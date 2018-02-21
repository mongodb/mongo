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
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/catalog_raii.h"
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
#include "mongo/db/logical_session_id.h"
#include "mongo/db/multi_key_path_tracker.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/prefetch.h"
#include "mongo/db/query/query_knobs.h"
#include "mongo/db/repl/apply_ops.h"
#include "mongo/db/repl/bgsync.h"
#include "mongo/db/repl/initial_syncer.h"
#include "mongo/db/repl/multiapplier.h"
#include "mongo/db/repl/oplogreader.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/repl_set_config.h"
#include "mongo/db/repl/replication_coordinator.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/repl/storage_interface.h"
#include "mongo/db/server_parameters.h"
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

AtomicInt32 SyncTail::replBatchLimitOperations{50 * 1000};

namespace {

/**
 * This variable determines the number of writer threads SyncTail will have. It can be overridden
 * using the "replWriterThreadCount" server parameter.
 */
int replWriterThreadCount = 16;

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
        return uassertStatusOK(UUID::parse(ui));
    }
    return nss;
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
                           OplogApplication::Mode oplogApplicationMode,
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

        // We convert updates to upserts when not in initial sync because after rollback and during
        // startup we may replay an update after a delete and crash since we do not ignore
        // errors. In initial sync we simply ignore these update errors so there is no reason to
        // upsert.
        //
        // TODO (SERVER-21700): Never upsert during oplog application unless an external applyOps
        // wants to. We should ignore these errors intelligently while in RECOVERING and STARTUP
        // mode (similar to initial sync) instead so we do not accidentally ignore real errors.
        bool shouldAlwaysUpsert = (oplogApplicationMode != OplogApplication::Mode::kInitialSync);
        Status status = applyOperationInLock(
            opCtx, db, op, shouldAlwaysUpsert, oplogApplicationMode, incrementOpsAppliedStats);
        if (!status.isOK() && status.code() == ErrorCodes::WriteConflict) {
            throw WriteConflictException();
        }
        return status;
    };

    bool isNoOp = opType[0] == 'n';
    if (isNoOp || (opType[0] == 'i' && nss.isSystemDotIndexes())) {
        if (isNoOp && nss.db() == "")
            return Status::OK();
        Lock::DBLock dbLock(opCtx, nss.db(), MODE_X);
        OldClientContext ctx(opCtx, nss.ns());
        return applyOp(ctx.db());
    }

    if (isCrudOpType(opType)) {
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
                // Delete operations on non-existent namespaces can be treated as successful for
                // idempotency reasons.
                if (opType[0] == 'd') {
                    return Status::OK();
                }
                ex.addContext(str::stream() << "Failed to apply operation: " << redact(op));
                throw;
            }
        });
    }

    if (opType[0] == 'c') {
        return writeConflictRetry(opCtx, "syncApply_command", nss.ns(), [&] {
            // a command may need a global write lock. so we will conservatively go
            // ahead and grab one here. suboptimal. :-(
            Lock::GlobalWrite globalWriteLock(opCtx);

            // special case apply for commands to avoid implicit database creation
            Status status = applyCommandInLock(opCtx, op, oplogApplicationMode);
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
                           OplogApplication::Mode oplogApplicationMode) {
    return SyncTail::syncApply(
        opCtx, op, oplogApplicationMode, applyOperation_inlock, applyCommand_inlock, [] {
            opsAppliedStats.increment(1);
        });
}


namespace {

// The pool threads call this to prefetch each op
void prefetchOp(const OplogEntry& oplogEntry) {
    initializePrefetchThread();

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
void prefetchOps(const MultiApplier::Operations& ops, OldThreadPool* prefetcherPool) {
    invariant(prefetcherPool);
    for (auto&& op : ops) {
        prefetcherPool->schedule([&] { prefetchOp(op); });
    }
    prefetcherPool->join();
}

// Doles out all the work to the writer pool threads.
// Does not modify writerVectors, but passes non-const pointers to inner vectors into func.
void applyOps(std::vector<MultiApplier::OperationPtrs>& writerVectors,
              OldThreadPool* writerPool,
              const MultiApplier::ApplyOperationFn& func,
              std::vector<Status>* statusVector,
              std::vector<WorkerMultikeyPathInfo>* workerMultikeyPathInfo) {
    invariant(writerVectors.size() == statusVector->size());
    for (size_t i = 0; i < writerVectors.size(); i++) {
        if (!writerVectors[i].empty()) {
            writerPool->schedule([&func, &writerVectors, statusVector, workerMultikeyPathInfo, i] {
                (*statusVector)[i] = func(&writerVectors[i], &((*workerMultikeyPathInfo)[i]));
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
                docs.emplace_back(InsertStatement{
                    ops[i].raw, ops[i].getOpTime().getTimestamp(), ops[i].getOpTime().getTerm()});
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

using SessionRecordMap =
    stdx::unordered_map<LogicalSessionId, SessionTxnRecord, LogicalSessionIdHash>;

void scheduleTxnTableUpdates(OperationContext* opCtx,
                             OldThreadPool* threadPool,
                             const SessionRecordMap& latestRecords) {
    for (const auto& it : latestRecords) {
        auto& record = it.second;

        threadPool->schedule([&record]() {
            initializeWriterThread();
            const auto opCtxHolder = cc().makeOperationContext();
            const auto opCtx = opCtxHolder.get();
            opCtx->lockState()->setShouldConflictWithSecondaryBatchApplication(false);

            Session::updateSessionRecordOnSecondary(opCtx, record);
        });
    }
}

/**
 * A session txn record is greater (i.e. later) than another if its transaction number is greater,
 * or if its transaction number is the same, and its last write optime is greater.
 *
 * Records can only be compared meaningfully if they are for the same session id.
 */
bool isSessionTxnRecordLaterThan(const SessionTxnRecord& lhs, const SessionTxnRecord& rhs) {
    invariant(lhs.getSessionId() == rhs.getSessionId());

    return (lhs.getTxnNum() > rhs.getTxnNum()) ||
        (lhs.getTxnNum() == rhs.getTxnNum() && lhs.getLastWriteOpTime() > rhs.getLastWriteOpTime());
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

/**
 * ops - This only modifies the isForCappedCollection field on each op. It does not alter the ops
 *      vector in any other way.
 * writerVectors - Set of operations for each worker thread to apply.
 * applyOpsOperations - If provided, stores extracted applyOps operations.
 */
void fillWriterVectors(OperationContext* opCtx,
                       MultiApplier::Operations* ops,
                       std::vector<MultiApplier::OperationPtrs>* writerVectors,
                       std::vector<MultiApplier::Operations>* applyOpsOperations) {
    const auto serviceContext = opCtx->getServiceContext();
    const auto storageEngine = serviceContext->getGlobalStorageEngine();

    const bool supportsDocLocking = storageEngine->supportsDocLocking();
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

        // Extract applyOps operations and fill writers with extracted operations using this
        // function.
        if (supportsDocLocking && op.isCommand() &&
            op.getCommandType() == OplogEntry::CommandType::kApplyOps) {
            try {
                applyOpsOperations->emplace_back(ApplyOps::extractOperations(op));
                fillWriterVectors(
                    opCtx, &applyOpsOperations->back(), writerVectors, applyOpsOperations);
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

/**
 * Returns a map of the "latest" transaction table records for each logical session id present in
 * the given operations. Each record represents the final state of the transaction table entry for
 * that session id after the operations are applied.
 */
SessionRecordMap getLatestSessionRecords(const MultiApplier::Operations& ops) {
    SessionRecordMap latestSessionRecords;

    for (auto&& op : ops) {
        const auto& sessionInfo = op.getOperationSessionInfo();
        if (sessionInfo.getTxnNumber()) {
            const auto& lsid = *sessionInfo.getSessionId();

            SessionTxnRecord record;
            record.setSessionId(lsid);
            record.setTxnNum(*sessionInfo.getTxnNumber());
            record.setLastWriteOpTime(op.getOpTime());
            invariant(op.getWallClockTime());
            record.setLastWriteDate(*op.getWallClockTime());

            auto it = latestSessionRecords.find(lsid);
            if (it == latestSessionRecords.end()) {
                latestSessionRecords.emplace(lsid, std::move(record));
            } else if (isSessionTxnRecordLaterThan(record, it->second)) {
                latestSessionRecords[lsid] = std::move(record);
            }
        }
    }

    return latestSessionRecords;
}

}  // namespace

OpTime SyncTail::multiApply_forTest(OperationContext* opCtx, MultiApplier::Operations ops) {
    return multiApply(opCtx, ops);
}
/**
 * Applies a batch of oplog entries by writing the oplog entries to the local oplog and then using
 * a set of threads to apply the operations. If the batch application is successful, returns the
 * optime of the last op applied, which should be the last op in the batch. To provide crash
 * resilience, this function will advance the persistent value of 'minValid' to at least the
 * last optime of the batch. If 'minValid' is already greater than or equal to the last optime of
 * this batch, it will not be updated.
 */
OpTime SyncTail::multiApply(OperationContext* opCtx, MultiApplier::Operations ops) {
    auto applyOperation = [this](MultiApplier::OperationPtrs* ops,
                                 WorkerMultikeyPathInfo* workerMultikeyPathInfo) -> Status {
        _applyFunc(ops, this, workerMultikeyPathInfo);
        // This function is used by 3.2 initial sync and steady state data replication.
        // _applyFunc() will throw or abort on error, so we return OK here.
        return Status::OK();
    };

    return fassertStatusOK(
        34437, repl::multiApply(opCtx, _writerPool.get(), std::move(ops), applyOperation));
}

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
    /**
     * Calculates batch limit size (in bytes) using the maximum capped collection size of the oplog
     * size.
     * Batches are limited to 10% of the oplog.
     */
    std::size_t _calculateBatchLimitBytes() {
        auto opCtx = cc().makeOperationContext();
        auto storageInterface = StorageInterface::get(opCtx.get());
        auto oplogMaxSizeResult =
            storageInterface->getOplogMaxSize(opCtx.get(), NamespaceString::kRsOplogNamespace);
        auto oplogMaxSize = fassertStatusOK(40301, oplogMaxSizeResult);
        return std::min(oplogMaxSize / 10, std::size_t(replBatchLimitBytes));
    }

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
        batchLimits.bytes = _calculateBatchLimitBytes();

        while (true) {
            batchLimits.slaveDelayLatestTimestamp = _calculateSlaveDelayLatestTimestamp();

            // Check this once per batch since users can change it at runtime.
            batchLimits.ops = replBatchLimitOperations.load();

            OpQueue ops;
            // tryPopAndWaitForMore adds to ops and returns true when we need to end a batch early.
            {
                auto opCtx = cc().makeOperationContext();
                while (!_syncTail->tryPopAndWaitForMore(opCtx.get(), &ops, batchLimits)) {
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

    // Get replication consistency markers.
    ReplicationProcess* replProcess = ReplicationProcess::get(replCoord->getServiceContext());
    ReplicationConsistencyMarkers* consistencyMarkers = replProcess->getConsistencyMarkers();
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
                if (_networkQueue->inShutdown()) {
                    severe() << "Turn off rsSyncApplyStop before attempting clean shutdown";
                    fassertFailedNoTrace(40304);
                }
                sleepmillis(10);
            }
        }

        // Get the current value of 'minValid'.
        minValid = consistencyMarkers->getMinValid(&opCtx);

        // Transition to SECONDARY state, if possible.
        tryToGoLiveAsASecondary(&opCtx, replCoord, minValid);

        long long termWhenBufferIsEmpty = replCoord->getTerm();
        // Blocks up to a second waiting for a batch to be ready to apply. If one doesn't become
        // ready in time, we'll loop again so we can do the above checks periodically.
        OpQueue ops = batcher.getNextBatch(Seconds(1));
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
        auto lastOpTimeAppliedInBatch = multiApply(&opCtx, ops.releaseBatch());
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
        consistencyMarkers->setAppliedThrough(&opCtx, lastOpTimeInBatch);

        // 3. Ensure that the last applied op time hasn't changed since the start of this batch.
        const auto lastAppliedOpTimeAtEndOfBatch = replCoord->getMyLastAppliedOpTime();
        invariant(lastAppliedOpTimeAtStartOfBatch == lastAppliedOpTimeAtEndOfBatch,
                  str::stream() << "the last known applied OpTime has changed from "
                                << lastAppliedOpTimeAtStartOfBatch.toString()
                                << " to "
                                << lastAppliedOpTimeAtEndOfBatch.toString()
                                << " in the middle of batch application");

        // 4. Finalize this batch. We are at a consistent optime if our current optime is >= the
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
    if (entry.isCommand() && entry.getCommandType() != OplogEntry::CommandType::kApplyOps) {
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

BSONObj SyncTail::getMissingDoc(OperationContext* opCtx, const OplogEntry& oplogEntry) {
    OplogReader missingObjReader;  // why are we using OplogReader to run a non-oplog query?

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
                str::stream() << "Can no longer connect to initial sync source: " << _hostname);
}

bool SyncTail::fetchAndInsertMissingDocument(OperationContext* opCtx,
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
            return false;
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

        return false;
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
                return false;
            }
            coll = db->getOrCreateCollection(opCtx, nss);
        } else {
            // If the oplog entry has a UUID, use it to find the collection in which to insert the
            // missing document.
            auto& catalog = UUIDCatalog::get(opCtx);
            coll = catalog.lookupCollectionByUUID(*uuid);
            if (!coll) {
                // TODO(SERVER-30819) insert this UUID into the missing UUIDs set.
                return false;
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
        return true;
    });
}

// This free function is used by the writer threads to apply each op
void multiSyncApply(MultiApplier::OperationPtrs* ops,
                    SyncTail* st,
                    WorkerMultikeyPathInfo* workerMultikeyPathInfo) {
    initializeWriterThread();
    auto opCtx = cc().makeOperationContext();
    auto syncApply = [](
        OperationContext* opCtx, const BSONObj& op, OplogApplication::Mode oplogApplicationMode) {
        return SyncTail::syncApply(opCtx, op, oplogApplicationMode);
    };
    {
        ON_BLOCK_EXIT(
            [&opCtx] { MultikeyPathTracker::get(opCtx.get()).stopTrackingMultikeyPathInfo(); });
        MultikeyPathTracker::get(opCtx.get()).startTrackingMultikeyPathInfo();
        fassertNoTrace(16359, multiSyncApply_noAbort(opCtx.get(), ops, syncApply));
    }

    invariant(!MultikeyPathTracker::get(opCtx.get()).isTrackingMultikeyPathInfo());
    invariant(workerMultikeyPathInfo->empty());
    auto newPaths = MultikeyPathTracker::get(opCtx.get()).getMultikeyPathInfo();
    if (!newPaths.empty()) {
        workerMultikeyPathInfo->swap(newPaths);
    }
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
    // TODO: This function can be called when we're in recovering as well as secondary. Set this
    // mode correctly.
    const OplogApplication::Mode oplogApplicationMode = OplogApplication::Mode::kSecondary;

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
                // We are going to group many 'i' ops into one big 'i' op, with array fields for
                // 'ts', 't', and 'o', corresponding to each individual op.
                // For example:
                // { ts: Timestamp(1,1), t:1, ns: "test.foo", op:"i", o: {_id:1} }
                // { ts: Timestamp(1,2), t:1, ns: "test.foo", op:"i", o: {_id:2} }
                // become:
                // { ts: [Timestamp(1, 1), Timestamp(1, 2)],
                //    t: [1, 1],
                //    o: [{_id: 1}, {_id: 2}],
                //   ns: "test.foo",
                //   op: "i" }
                BSONObjBuilder groupedInsertBuilder;

                // Populate the "ts" field with an array of all the grouped inserts' timestamps.
                BSONArrayBuilder tsArrayBuilder(groupedInsertBuilder.subarrayStart("ts"));
                for (auto groupingIterator = oplogEntriesIterator;
                     groupingIterator != endOfGroupableOpsIterator;
                     ++groupingIterator) {
                    tsArrayBuilder.append((*groupingIterator)->getTimestamp());
                }
                tsArrayBuilder.done();

                // Populate the "t" (term) field with an array of all the grouped inserts' terms.
                BSONArrayBuilder tArrayBuilder(groupedInsertBuilder.subarrayStart("t"));
                for (auto groupingIterator = oplogEntriesIterator;
                     groupingIterator != endOfGroupableOpsIterator;
                     ++groupingIterator) {
                    auto parsedTerm = (*groupingIterator)->getTerm();
                    long long term = OpTime::kUninitializedTerm;
                    // Term may not be present (pv0)
                    if (parsedTerm) {
                        term = parsedTerm.get();
                    }
                    tArrayBuilder.append(term);
                }
                tArrayBuilder.done();

                // Generate an op object of all elements except for "ts", "t", and "o", since we
                // need to make those fields arrays of all the ts's, t's, and o's.
                for (auto elem : entry->raw) {
                    if (elem.fieldNameStringData() != "o" && elem.fieldNameStringData() != "ts" &&
                        elem.fieldNameStringData() != "t") {
                        groupedInsertBuilder.append(elem);
                    }
                }

                // Populate the "o" field with an array of all the grouped inserts.
                BSONArrayBuilder oArrayBuilder(groupedInsertBuilder.subarrayStart("o"));
                for (auto groupingIterator = oplogEntriesIterator;
                     groupingIterator != endOfGroupableOpsIterator;
                     ++groupingIterator) {
                    oArrayBuilder.append((*groupingIterator)->getObject());
                }
                oArrayBuilder.done();

                try {
                    // Apply the group of inserts.
                    uassertStatusOK(
                        syncApply(opCtx, groupedInsertBuilder.done(), oplogApplicationMode));
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
            const Status status = syncApply(opCtx, entry->raw, oplogApplicationMode);

            if (!status.isOK()) {
                severe() << "Error applying operation (" << redact(entry->toBSON())
                         << "): " << causedBy(redact(status));
                return status;
            }
        } catch (const DBException& e) {
            severe() << "writer worker caught exception: " << redact(e)
                     << " on: " << redact(entry->toBSON());
            return e.toStatus();
        }
    }

    return Status::OK();
}

Status multiInitialSyncApply(MultiApplier::OperationPtrs* ops,
                             SyncTail* st,
                             AtomicUInt32* fetchCount,
                             WorkerMultikeyPathInfo* workerMultikeyPathInfo) {
    initializeWriterThread();
    auto opCtx = cc().makeOperationContext();
    return multiInitialSyncApply_noAbort(opCtx.get(), ops, st, fetchCount, workerMultikeyPathInfo);
}

Status multiInitialSyncApply_noAbort(OperationContext* opCtx,
                                     MultiApplier::OperationPtrs* ops,
                                     SyncTail* st,
                                     AtomicUInt32* fetchCount,
                                     WorkerMultikeyPathInfo* workerMultikeyPathInfo) {
    UnreplicatedWritesBlock uwb(opCtx);
    DisableDocumentValidation validationDisabler(opCtx);
    {  // Ensure that the MultikeyPathTracker stops tracking paths.
        ON_BLOCK_EXIT([opCtx] { MultikeyPathTracker::get(opCtx).stopTrackingMultikeyPathInfo(); });
        MultikeyPathTracker::get(opCtx).startTrackingMultikeyPathInfo();

        // allow us to get through the magic barrier
        opCtx->lockState()->setShouldConflictWithSecondaryBatchApplication(false);

        for (auto it = ops->begin(); it != ops->end(); ++it) {
            auto& entry = **it;
            try {
                const Status s =
                    SyncTail::syncApply(opCtx, entry.raw, OplogApplication::Mode::kInitialSync);
                if (!s.isOK()) {
                    // In initial sync, update operations can cause documents to be missed during
                    // collection cloning. As a result, it is possible that a document that we need
                    // to update is not present locally. In that case we fetch the document from the
                    // sync source.
                    if (s != ErrorCodes::UpdateOperationFailed) {
                        error() << "Error applying operation: " << redact(s) << " ("
                                << redact(entry.toBSON()) << ")";
                        return s;
                    }

                    // We might need to fetch the missing docs from the sync source.
                    fetchCount->fetchAndAdd(1);
                    st->fetchAndInsertMissingDocument(opCtx, entry);
                }
            } catch (const DBException& e) {
                // SERVER-24927 If we have a NamespaceNotFound exception, then this document will be
                // dropped before initial sync ends anyways and we should ignore it.
                if (e.code() == ErrorCodes::NamespaceNotFound && entry.isCrudOpType()) {
                    continue;
                }

                severe() << "writer worker caught exception: " << causedBy(redact(e))
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

    const auto storageEngine = opCtx->getServiceContext()->getGlobalStorageEngine();
    if (storageEngine->isMmapV1()) {
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
    std::vector<WorkerMultikeyPathInfo> multikeyVector(workerPool->getNumThreads());
    {
        // Each node records cumulative batch application stats for itself using this timer.
        TimerHolder timer(&applyBatchStats);

        // We must wait for the all work we've dispatched to complete before leaving this block
        // because the spawned threads refer to objects on the stack
        ON_BLOCK_EXIT([&] { workerPool->join(); });

        // Write batch of ops into oplog.
        consistencyMarkers->setOplogTruncateAfterPoint(opCtx, ops.front().getTimestamp());
        scheduleWritesToOplog(opCtx, workerPool, ops);

        // Holds extracted applyOps operations. Keep in scope until all operations in 'ops' and
        // 'applyOpsOperations' have been applied.
        std::vector<MultiApplier::Operations> applyOpsOperations;

        std::vector<MultiApplier::OperationPtrs> writerVectors(workerPool->getNumThreads());
        fillWriterVectors(opCtx, &ops, &writerVectors, &applyOpsOperations);

        // Wait for writes to finish before applying ops.
        workerPool->join();

        // Reset consistency markers in case the node fails while applying ops.
        consistencyMarkers->setOplogTruncateAfterPoint(opCtx, Timestamp());
        consistencyMarkers->setMinValidToAtLeast(opCtx, ops.back().getOpTime());

        applyOps(writerVectors, workerPool, applyOperation, &statusVector, &multikeyVector);
        workerPool->join();

        // Update the transaction table to point to the latest oplog entries for each session id.
        const auto latestSessionRecords = getLatestSessionRecords(ops);
        scheduleTxnTableUpdates(opCtx, workerPool, latestSessionRecords);
        workerPool->join();

        // Notify the storage engine that a replication batch has completed.
        // This means that all the writes associated with the oplog entries in the batch are
        // finished and no new writes with timestamps associated with those oplog entries will show
        // up in the future.
        const auto storageEngine = opCtx->getServiceContext()->getGlobalStorageEngine();
        storageEngine->replicationBatchIsComplete();
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
            fassertStatusOK(
                50686,
                StorageInterface::get(opCtx)->setIndexIsMultikey(
                    opCtx, info.nss, info.indexName, info.multikeyPaths, firstTimeInBatch));
        }
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
