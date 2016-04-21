/**
 *    Copyright (C) 2013 10gen Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kWrite

#include "mongo/platform/basic.h"

#include "mongo/db/commands/write_commands/batch_executor.h"

#include <memory>

#include "mongo/base/error_codes.h"
#include "mongo/db/catalog/database.h"
#include "mongo/db/catalog/database_holder.h"
#include "mongo/db/catalog/document_validation.h"
#include "mongo/db/catalog/index_create.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop_metrics.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/delete.h"
#include "mongo/db/exec/update.h"
#include "mongo/db/instance.h"
#include "mongo/db/introspect.h"
#include "mongo/db/lasterror.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/op_observer.h"
#include "mongo/db/ops/delete_request.h"
#include "mongo/db/ops/insert.h"
#include "mongo/db/ops/parsed_delete.h"
#include "mongo/db/ops/parsed_update.h"
#include "mongo/db/ops/update_lifecycle_impl.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/query/query_knobs.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/repl/repl_client_info.h"
#include "mongo/db/repl/repl_settings.h"
#include "mongo/db/repl/replication_coordinator_global.h"
#include "mongo/db/server_parameters.h"
#include "mongo/db/s/collection_metadata.h"
#include "mongo/db/s/collection_sharding_state.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/sharded_connection_info.h"
#include "mongo/db/s/sharding_state.h"
#include "mongo/db/stats/counters.h"
#include "mongo/db/stats/top.h"
#include "mongo/db/write_concern.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/shard_key_pattern.h"
#include "mongo/s/stale_exception.h"
#include "mongo/s/write_ops/batched_upsert_detail.h"
#include "mongo/s/write_ops/write_error_detail.h"
#include "mongo/stdx/memory.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/elapsed_tracker.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

using std::string;
using std::unique_ptr;
using std::vector;
using str::stream;

namespace {

/**
 * Data structure to safely hold and clean up results of single write operations.
 */
class WriteOpResult {
    MONGO_DISALLOW_COPYING(WriteOpResult);

public:
    WriteOpResult() {}

    WriteOpStats& getStats() {
        return _stats;
    }

    WriteErrorDetail* getError() {
        return _error.get();
    }
    WriteErrorDetail* releaseError() {
        return _error.release();
    }
    void setError(WriteErrorDetail* error) {
        _error.reset(error);
    }

private:
    WriteOpStats _stats;
    std::unique_ptr<WriteErrorDetail> _error;
};

WriteErrorDetail* toWriteError(const Status& status) {
    WriteErrorDetail* error = new WriteErrorDetail;

    // TODO: Complex transform here?
    error->setErrCode(status.code());
    error->setErrMessage(status.reason());

    return error;
}

void toBatchError(const Status& status, BatchedCommandResponse* response) {
    response->clear();
    response->setErrCode(status.code());
    response->setErrMessage(status.reason());
    response->setOk(false);
    dassert(response->isValid(NULL));
}

/**
 * Translates write item type to wire protocol op code. Helper for
 * WriteBatchExecutor::applyWriteItem().
 */
NetworkOp getOpCode(const BatchItemRef& currWrite) {
    switch (currWrite.getRequest()->getBatchType()) {
        case BatchedCommandRequest::BatchType_Insert:
            return dbInsert;
        case BatchedCommandRequest::BatchType_Update:
            return dbUpdate;
        case BatchedCommandRequest::BatchType_Delete:
            return dbDelete;
        default:
            MONGO_UNREACHABLE;
    }
}

void buildStaleError(const ChunkVersion& shardVersionRecvd,
                     const ChunkVersion& shardVersionWanted,
                     WriteErrorDetail* error) {
    // Write stale error to results
    error->setErrCode(ErrorCodes::StaleShardVersion);

    BSONObjBuilder infoB;
    shardVersionWanted.addToBSON(infoB, "vWanted");
    error->setErrInfo(infoB.obj());

    string errMsg = stream() << "stale shard version detected before write, received "
                             << shardVersionRecvd.toString() << " but local version is "
                             << shardVersionWanted.toString();
    error->setErrMessage(errMsg);
}

bool checkShardVersion(OperationContext* txn,
                       const BatchedCommandRequest& request,
                       WriteOpResult* result) {
    const auto& css = CollectionShardingState::get(txn, request.getTargetingNSS());

    try {
        css->checkShardVersionOrThrow(txn);
        return true;
    } catch (const StaleConfigException& e) {
        result->setError(new WriteErrorDetail());
        buildStaleError(e.getVersionReceived(), e.getVersionWanted(), result->getError());
        return false;
    }
}

}  // namespace

WriteBatchExecutor::WriteBatchExecutor(OperationContext* txn, OpCounters* opCounters, LastError* le)
    : _txn(txn), _opCounters(opCounters), _le(le), _stats(new WriteBatchStats) {}

// static
Status WriteBatchExecutor::validateBatch(const BatchedCommandRequest& request) {
    // Validate namespace
    const NamespaceString& nss = request.getNS();
    if (!nss.isValid()) {
        return Status(ErrorCodes::InvalidNamespace, nss.ns() + " is not a valid namespace");
    }

    // Make sure we can write to the namespace
    Status allowedStatus = userAllowedWriteNS(nss);
    if (!allowedStatus.isOK()) {
        return allowedStatus;
    }

    // Validate insert index requests
    // TODO: Push insert index requests through createIndex once all upgrade paths support it
    string errMsg;
    if (request.isInsertIndexRequest() && !request.isValidIndexRequest(&errMsg)) {
        return Status(ErrorCodes::InvalidOptions, errMsg);
    }

    return Status::OK();
}

void WriteBatchExecutor::executeBatch(const BatchedCommandRequest& request,
                                      BatchedCommandResponse* response) {
    // Validate namespace
    Status isValid = validateBatch(request);
    if (!isValid.isOK()) {
        toBatchError(isValid, response);
        return;
    }

    if (request.sizeWriteOps() == 0u) {
        toBatchError(Status(ErrorCodes::InvalidLength, "no write ops were included in the batch"),
                     response);
        return;
    }

    // Validate batch size
    if (request.sizeWriteOps() > BatchedCommandRequest::kMaxWriteBatchSize) {
        toBatchError(Status(ErrorCodes::InvalidLength,
                            stream() << "exceeded maximum write batch size of "
                                     << BatchedCommandRequest::kMaxWriteBatchSize),
                     response);
        return;
    }

    //
    // End validation
    //

    const WriteConcernOptions& writeConcern = _txn->getWriteConcern();
    bool silentWC = writeConcern.wMode.empty() && writeConcern.wNumNodes == 0 &&
        (writeConcern.syncMode == WriteConcernOptions::SyncMode::NONE ||
         writeConcern.syncMode == WriteConcernOptions::SyncMode::UNSET);

    Timer commandTimer;

    OwnedPointerVector<WriteErrorDetail> writeErrorsOwned;
    vector<WriteErrorDetail*>& writeErrors = writeErrorsOwned.mutableVector();

    OwnedPointerVector<BatchedUpsertDetail> upsertedOwned;
    vector<BatchedUpsertDetail*>& upserted = upsertedOwned.mutableVector();

    //
    // Apply each batch item, possibly bulking some items together in the write lock.
    // Stops on error if batch is ordered.
    //

    bulkExecute(request, &upserted, &writeErrors);

    //
    // Refresh metadata if needed
    //

    const bool staleBatch =
        !writeErrors.empty() && writeErrors.back()->getErrCode() == ErrorCodes::StaleShardVersion;

    if (staleBatch) {
        const auto& oss = OperationShardingState::get(_txn);

        ChunkVersion requestedShardVersion = oss.getShardVersion(request.getTargetingNSS());
        ShardingState::get(_txn)
            ->onStaleShardVersion(_txn, request.getTargetingNSS(), requestedShardVersion);
    }

    //
    // Construct response
    //

    response->setOk(true);

    if (!silentWC) {
        if (upserted.size()) {
            response->setUpsertDetails(upserted);
        }

        if (writeErrors.size()) {
            response->setErrDetails(writeErrors);
        }

        repl::ReplicationCoordinator* replCoord = repl::getGlobalReplicationCoordinator();
        const repl::ReplicationCoordinator::Mode replMode = replCoord->getReplicationMode();
        if (replMode != repl::ReplicationCoordinator::modeNone) {
            response->setLastOp(repl::ReplClientInfo::forClient(_txn->getClient()).getLastOp());
            if (replMode == repl::ReplicationCoordinator::modeReplSet) {
                response->setElectionId(replCoord->getElectionId());
            }
        }

        // Set the stats for the response
        response->setN(_stats->numInserted + _stats->numUpserted + _stats->numMatched +
                       _stats->numDeleted);
        if (request.getBatchType() == BatchedCommandRequest::BatchType_Update)
            response->setNModified(_stats->numModified);
    }

    dassert(response->isValid(NULL));
}

static bool checkIsMasterForDatabase(const NamespaceString& ns, WriteOpResult* result) {
    if (!repl::getGlobalReplicationCoordinator()->canAcceptWritesFor(ns)) {
        WriteErrorDetail* errorDetail = new WriteErrorDetail;
        result->setError(errorDetail);
        errorDetail->setErrCode(ErrorCodes::NotMaster);
        errorDetail->setErrMessage("Not primary while writing to " + ns.toString());
        return false;
    }
    return true;
}

static void buildUniqueIndexError(const BSONObj& keyPattern,
                                  const BSONObj& indexPattern,
                                  WriteErrorDetail* error) {
    error->setErrCode(ErrorCodes::CannotCreateIndex);
    string errMsg = stream() << "cannot create unique index over " << indexPattern
                             << " with shard key pattern " << keyPattern;
    error->setErrMessage(errMsg);
}

static bool checkIndexConstraints(OperationContext* txn,
                                  const BatchedCommandRequest& request,
                                  WriteOpResult* result) {
    const NamespaceString& nss = request.getTargetingNSS();
    dassert(txn->lockState()->isCollectionLockedForMode(nss.ns(), MODE_IX));

    if (!request.isUniqueIndexRequest())
        return true;

    ShardingState* shardingState = ShardingState::get(txn);
    if (shardingState->enabled()) {
        auto metadata = shardingState->getCollectionMetadata(nss.ns());
        if (metadata) {
            ShardKeyPattern shardKeyPattern(metadata->getKeyPattern());
            if (!shardKeyPattern.isUniqueIndexCompatible(request.getIndexKeyPattern())) {
                result->setError(new WriteErrorDetail);
                buildUniqueIndexError(
                    metadata->getKeyPattern(), request.getIndexKeyPattern(), result->getError());

                return false;
            }
        }
    }

    return true;
}

//
// HELPERS FOR CUROP MANAGEMENT AND GLOBAL STATS
//

static void beginCurrentOp(OperationContext* txn, const BatchItemRef& currWrite) {
    stdx::lock_guard<Client> lk(*txn->getClient());
    CurOp* const currentOp = CurOp::get(txn);
    currentOp->setNetworkOp_inlock(getOpCode(currWrite));
    currentOp->setLogicalOp_inlock(networkOpToLogicalOp(getOpCode(currWrite)));
    currentOp->ensureStarted();
    currentOp->setNS_inlock(currWrite.getRequest()->getNS().ns());

    if (currWrite.getOpType() == BatchedCommandRequest::BatchType_Insert) {
        currentOp->setQuery_inlock(currWrite.getDocument());
        currentOp->debug().query = currWrite.getDocument();
        currentOp->debug().ninserted = 0;
    } else if (currWrite.getOpType() == BatchedCommandRequest::BatchType_Update) {
        currentOp->setQuery_inlock(currWrite.getUpdate()->getQuery());
        currentOp->debug().query = currWrite.getUpdate()->getQuery();
        currentOp->debug().updateobj = currWrite.getUpdate()->getUpdateExpr();
        // Note: debug().nMatched, nModified and nmoved are set internally in update
    } else {
        dassert(currWrite.getOpType() == BatchedCommandRequest::BatchType_Delete);
        currentOp->setQuery_inlock(currWrite.getDelete()->getQuery());
        currentOp->debug().query = currWrite.getDelete()->getQuery();
        currentOp->debug().ndeleted = 0;
    }
}

void WriteBatchExecutor::incWriteStats(const BatchedCommandRequest::BatchType opType,
                                       const WriteOpStats& stats,
                                       const WriteErrorDetail* error,
                                       CurOp* currentOp) {
    if (opType == BatchedCommandRequest::BatchType_Update) {
        if (stats.upsertedID.isEmpty()) {
            _stats->numMatched += stats.n;
            _stats->numModified += stats.nModified;
        } else {
            ++_stats->numUpserted;
        }

        if (!error) {
            _le->recordUpdate(stats.upsertedID.isEmpty() && stats.n > 0, stats.n, stats.upsertedID);
        }
    } else {
        dassert(opType == BatchedCommandRequest::BatchType_Delete);
        _stats->numDeleted += stats.n;
        if (!error) {
            _le->recordDelete(stats.n);
        }
        currentOp->debug().ndeleted += stats.n;
    }

    if (error) {
        _le->setLastError(error->getErrCode(), error->getErrMessage().c_str());
    }
}

static void logCurOpError(CurOp* currentOp, WriteErrorDetail* opError) {
    invariant(opError != nullptr);
    currentOp->debug().exceptionInfo =
        ExceptionInfo(opError->getErrMessage(), opError->getErrCode());

    LOG(3) << " Caught Assertion in " << networkOpToString(currentOp->getNetworkOp())
           << ", continuing " << causedBy(opError->getErrMessage());
}

static void finishCurrentOp(OperationContext* txn, WriteErrorDetail* opError) {
    CurOp* currentOp = CurOp::get(txn);
    currentOp->done();
    int executionTime = currentOp->debug().executionTime = currentOp->totalTimeMillis();
    recordCurOpMetrics(txn);
    Top::get(txn->getClient()->getServiceContext())
        .record(currentOp->getNS(),
                currentOp->getLogicalOp(),
                1,  // "write locked"
                currentOp->totalTimeMicros(),
                currentOp->isCommand());

    if (opError)
        logCurOpError(currentOp, opError);

    bool logAll = logger::globalLogDomain()->shouldLog(logger::LogComponent::kCommand,
                                                       logger::LogSeverity::Debug(1));
    bool logSlow = executionTime > (serverGlobalParams.slowMS + currentOp->getExpectedLatencyMs());

    if (logAll || logSlow) {
        Locker::LockerInfo lockerInfo;
        txn->lockState()->getLockerInfo(&lockerInfo);

        LOG(0) << currentOp->debug().report(*currentOp, lockerInfo.stats);
    }

    if (currentOp->shouldDBProfile(executionTime)) {
        profile(txn, CurOp::get(txn)->getNetworkOp());
    }
}

// END HELPERS

//
// CORE WRITE OPERATIONS (declaration)
// These functions write to the database and return stats and zero or one of:
// - page fault
// - error
//

static void singleCreateIndex(OperationContext* txn,
                              const BSONObj& indexDesc,
                              WriteOpResult* result);

static void multiUpdate(OperationContext* txn,
                        const BatchItemRef& updateItem,
                        WriteOpResult* result);

static void multiRemove(OperationContext* txn,
                        const BatchItemRef& removeItem,
                        WriteOpResult* result);

//
// WRITE EXECUTION
// In general, the exec* operations manage db lock state and stats before dispatching to the
// core write operations, which are *only* responsible for performing a write and reporting
// success or failure.
//

/**
 * Representation of the execution state of execInserts.  Used by a single
 * execution of execInserts in a single thread.
 */
class WriteBatchExecutor::ExecInsertsState {
    MONGO_DISALLOW_COPYING(ExecInsertsState);

public:
    /**
     * Constructs a new instance, for performing inserts described in "aRequest".
     */
    explicit ExecInsertsState(OperationContext* txn, const BatchedCommandRequest* aRequest);

    /**
     * Acquires the write lock and client context needed to perform the current write operation.
     * Returns true on success, after which it is safe to use the "context" and "collection"
     * members.  It is safe to call this function if this instance already holds the write lock.
     *
     * On failure, writeLock, context and collection will be NULL/clear.
     */
    bool lockAndCheck(WriteOpResult* result);

    /**
     * Releases the client context and write lock acquired by lockAndCheck.  Safe to call
     * regardless of whether or not this state object currently owns the lock.
     */
    void unlock();

    /**
     * Returns true if this executor has the lock on the target database.
     */
    bool hasLock() {
        return _dbLock.get();
    }

    /**
     * Gets the target collection for the batch operation.  Value is undefined
     * unless hasLock() is true.
     */
    Collection* getCollection() {
        return _collection;
    }

    OperationContext* txn;

    // Request object describing the inserts.
    const BatchedCommandRequest* request;

    // Index of the current insert operation to perform.
    size_t currIndex = 0;

    // Translation of insert documents in "request" into insert-ready forms.  This vector has a
    // correspondence with elements of the "request", and "currIndex" is used to
    // index both.
    std::vector<StatusWith<BSONObj>> normalizedInserts;

private:
    bool _lockAndCheckImpl(WriteOpResult* result, bool intentLock);

    ScopedTransaction _transaction;
    // Guard object for the write lock on the target database.
    std::unique_ptr<Lock::DBLock> _dbLock;
    std::unique_ptr<Lock::CollectionLock> _collLock;

    Database* _database = nullptr;
    Collection* _collection = nullptr;
};

void WriteBatchExecutor::bulkExecute(const BatchedCommandRequest& request,
                                     std::vector<BatchedUpsertDetail*>* upsertedIds,
                                     std::vector<WriteErrorDetail*>* errors) {
    boost::optional<DisableDocumentValidation> maybeDisableValidation;
    if (request.shouldBypassValidation()) {
        maybeDisableValidation.emplace(_txn);
    }

    if (request.getBatchType() == BatchedCommandRequest::BatchType_Insert) {
        execInserts(request, errors);
    } else if (request.getBatchType() == BatchedCommandRequest::BatchType_Update) {
        for (size_t i = 0; i < request.sizeWriteOps(); i++) {
            if (i + 1 == request.sizeWriteOps()) {
                setupSynchronousCommit(_txn);
            }

            WriteErrorDetail* error = NULL;
            BSONObj upsertedId;
            execUpdate(BatchItemRef(&request, i), &upsertedId, &error);

            if (!upsertedId.isEmpty()) {
                BatchedUpsertDetail* batchUpsertedId = new BatchedUpsertDetail;
                batchUpsertedId->setIndex(i);
                batchUpsertedId->setUpsertedID(upsertedId);
                upsertedIds->push_back(batchUpsertedId);
            }

            if (error) {
                errors->push_back(error);
                if (request.getOrdered())
                    break;
            }
        }
    } else {
        dassert(request.getBatchType() == BatchedCommandRequest::BatchType_Delete);
        for (size_t i = 0; i < request.sizeWriteOps(); i++) {
            if (i + 1 == request.sizeWriteOps()) {
                setupSynchronousCommit(_txn);
            }

            WriteErrorDetail* error = NULL;
            execRemove(BatchItemRef(&request, i), &error);

            if (error) {
                errors->push_back(error);
                if (request.getOrdered())
                    break;
            }
        }
    }

    // Fill in stale version errors for unordered batches (update/delete can't do this on own)
    if (!errors->empty() && !request.getOrdered()) {
        const WriteErrorDetail* finalError = errors->back();

        if (finalError->getErrCode() == ErrorCodes::StaleShardVersion) {
            for (size_t i = finalError->getIndex() + 1; i < request.sizeWriteOps(); i++) {
                WriteErrorDetail* dupStaleError = new WriteErrorDetail;
                finalError->cloneTo(dupStaleError);
                errors->push_back(dupStaleError);
            }
        }
    }
}

// Goes over the request and preprocesses normalized versions of all the inserts in the request
static void normalizeInserts(const BatchedCommandRequest& request,
                             vector<StatusWith<BSONObj>>* normalizedInserts) {
    normalizedInserts->reserve(request.sizeWriteOps());
    for (size_t i = 0; i < request.sizeWriteOps(); ++i) {
        BSONObj insertDoc = request.getInsertRequest()->getDocumentsAt(i);
        StatusWith<BSONObj> normalInsert = fixDocumentForInsert(insertDoc);
        normalizedInserts->push_back(normalInsert);
        if (request.getOrdered() && !normalInsert.isOK())
            break;
    }
}

static void insertOne(WriteBatchExecutor::ExecInsertsState* state, WriteOpResult* result);

// Loops over the specified subset of the batch, processes one document at a time.
// Returns a true to discontinue the insert, or false if not.
bool WriteBatchExecutor::insertMany(WriteBatchExecutor::ExecInsertsState* state,
                                    size_t startIndex,
                                    size_t endIndex,
                                    CurOp* currentOp,
                                    std::vector<WriteErrorDetail*>* errors,
                                    bool ordered) {
    for (state->currIndex = startIndex; state->currIndex < endIndex; ++state->currIndex) {
        WriteOpResult result;
        BatchItemRef currInsertItem(state->request, state->currIndex);
        {
            stdx::lock_guard<Client> lk(*_txn->getClient());
            currentOp->setQuery_inlock(currInsertItem.getDocument());
            currentOp->debug().query = currInsertItem.getDocument();
        }

        _opCounters->gotInsert();
        // Internally, insertOne retries the single insert until it completes without a write
        // conflict exception, or until it fails with some kind of error.  Errors are mostly
        // propagated via the request->error field, but DBExceptions or std::exceptions may escape,
        // particularly on operation interruption.  These kinds of errors necessarily prevent
        // further insertOne calls, and stop the batch.  As a result, the only expected source of
        // such exceptions are interruptions.
        insertOne(state, &result);

        uint64_t nInserted = result.getStats().n;
        _stats->numInserted += nInserted;
        currentOp->debug().ninserted += nInserted;

        const WriteErrorDetail* error = result.getError();
        if (error) {
            _le->setLastError(error->getErrCode(), error->getErrMessage().c_str());
            WriteErrorDetail* error = NULL;
            error = result.releaseError();
            errors->push_back(error);
            error->setIndex(state->currIndex);
            logCurOpError(CurOp::get(_txn), error);
            if (ordered)
                return true;
        } else {
            _le->recordInsert(nInserted);
        }
    }
    return false;
}

// Instantiates an ExecInsertsState, which represents all of the state for the batch.
// Breaks out into manageably sized chunks for insertMany, between which we can yield.
// Encapsulates the lock state.
void WriteBatchExecutor::execInserts(const BatchedCommandRequest& request,
                                     std::vector<WriteErrorDetail*>* errors) {
    ExecInsertsState state(_txn, &request);
    normalizeInserts(request, &state.normalizedInserts);

    CurOp* currentOp;
    {
        stdx::lock_guard<Client> lk(*_txn->getClient());
        currentOp = CurOp::get(_txn);
        currentOp->setLogicalOp_inlock(LogicalOp::opInsert);
        currentOp->ensureStarted();
        currentOp->setNS_inlock(request.getNS().ns());
        currentOp->debug().ninserted = 0;
    }

    auto client = _txn->getClient();
    auto lastOpAtOperationStart = repl::ReplClientInfo::forClient(client).getLastOp();
    ScopeGuard lastOpSetterGuard = MakeObjGuard(repl::ReplClientInfo::forClient(client),
                                                &repl::ReplClientInfo::setLastOpToSystemLastOpTime,
                                                _txn);

    // If this is the local database, don't set last op.
    if (request.getNS().isLocal()) {
        lastOpSetterGuard.Dismiss();
    }

    int64_t chunkCount = 0;
    int64_t chunkBytes = 0;
    const int64_t chunkMaxCount = internalQueryExecYieldIterations / 2;
    size_t startIndex = 0;
    size_t maxIndex = state.request->sizeWriteOps() - 1;

    for (size_t i = 0; i <= maxIndex; ++i) {
        if (i == maxIndex)
            setupSynchronousCommit(_txn);
        state.currIndex = i;
        BatchItemRef currInsertItem(state.request, state.currIndex);
        chunkBytes += currInsertItem.getDocument().objsize();
        chunkCount++;

        if ((chunkCount >= chunkMaxCount) || (chunkBytes >= insertVectorMaxBytes) ||
            (i == maxIndex)) {
            bool stop;
            stop = insertMany(&state, startIndex, i + 1, currentOp, errors, request.getOrdered());
            startIndex = i + 1;
            chunkCount = 0;
            chunkBytes = 0;

            if (state.hasLock()) {
                // insertOne acquires the locks, but does not release them on non-error cases,
                // so we release them here. insertOne() reacquires them via lockAndCheck().
                state.unlock();
                // This releases any storage engine held locks/snapshots.
                _txn->recoveryUnit()->abandonSnapshot();
                // Since the lock manager guarantees FIFO queues waiting on locks,
                // there is no need to explicitly sleep or give up control of the processor here.
            }
            if (stop)
                break;
        }
    }

    if (repl::ReplClientInfo::forClient(client).getLastOp() != lastOpAtOperationStart) {
        // If this operation has already generated a new lastOp, don't bother setting it
        // here. No-op updates will not generate a new lastOp, so we still need the guard to
        // fire in that case.
        lastOpSetterGuard.Dismiss();
    }

    // TODO: Move Top and CurOp metrics management into an RAII object.
    currentOp->done();
    recordCurOpMetrics(_txn);
    Top::get(_txn->getClient()->getServiceContext())
        .record(currentOp->getNS(),
                currentOp->getLogicalOp(),
                1,  // "write locked"
                currentOp->totalTimeMicros(),
                currentOp->isCommand());
}

void WriteBatchExecutor::execUpdate(const BatchItemRef& updateItem,
                                    BSONObj* upsertedId,
                                    WriteErrorDetail** error) {
    // BEGIN CURRENT OP
    CurOp currentOp(_txn);
    beginCurrentOp(_txn, updateItem);
    _opCounters->gotUpdate();

    WriteOpResult result;
    multiUpdate(_txn, updateItem, &result);

    if (!result.getStats().upsertedID.isEmpty()) {
        *upsertedId = result.getStats().upsertedID;
    }
    // END CURRENT OP
    incWriteStats(updateItem.getOpType(), result.getStats(), result.getError(), &currentOp);
    finishCurrentOp(_txn, result.getError());

    // End current transaction and release snapshot.
    _txn->recoveryUnit()->abandonSnapshot();

    if (result.getError()) {
        result.getError()->setIndex(updateItem.getItemIndex());
        *error = result.releaseError();
    }
}

void WriteBatchExecutor::execRemove(const BatchItemRef& removeItem, WriteErrorDetail** error) {
    // Removes are similar to updates, but page faults are handled externally

    // BEGIN CURRENT OP
    CurOp currentOp(_txn);
    beginCurrentOp(_txn, removeItem);
    _opCounters->gotDelete();

    WriteOpResult result;
    multiRemove(_txn, removeItem, &result);

    // END CURRENT OP
    incWriteStats(removeItem.getOpType(), result.getStats(), result.getError(), &currentOp);
    finishCurrentOp(_txn, result.getError());

    // End current transaction and release snapshot.
    _txn->recoveryUnit()->abandonSnapshot();

    if (result.getError()) {
        result.getError()->setIndex(removeItem.getItemIndex());
        *error = result.releaseError();
    }
}

//
// IN-DB-LOCK CORE OPERATIONS
//

WriteBatchExecutor::ExecInsertsState::ExecInsertsState(OperationContext* txn,
                                                       const BatchedCommandRequest* aRequest)
    : txn(txn), request(aRequest), _transaction(txn, MODE_IX) {}

bool WriteBatchExecutor::ExecInsertsState::_lockAndCheckImpl(WriteOpResult* result,
                                                             bool intentLock) {
    if (hasLock()) {
        CurOp::get(txn)->raiseDbProfileLevel(_database->getProfilingLevel());
        return true;
    }

    if (request->isInsertIndexRequest())
        intentLock = false;  // can't build indexes in intent mode

    const NamespaceString& nss = request->getNS();
    invariant(!_collLock);
    invariant(!_dbLock);
    _dbLock =
        stdx::make_unique<Lock::DBLock>(txn->lockState(), nss.db(), intentLock ? MODE_IX : MODE_X);
    _database = dbHolder().get(txn, nss.ns());
    if (intentLock && !_database) {
        // Ensure exclusive lock in case the database doesn't yet exist
        _dbLock.reset();
        _dbLock = stdx::make_unique<Lock::DBLock>(txn->lockState(), nss.db(), MODE_X);
        intentLock = false;
    }
    _collLock = stdx::make_unique<Lock::CollectionLock>(
        txn->lockState(), nss.ns(), intentLock ? MODE_IX : MODE_X);
    if (!checkIsMasterForDatabase(nss, result)) {
        return false;
    }
    if (!checkShardVersion(txn, *request, result)) {
        return false;
    }
    if (!checkIndexConstraints(txn, *request, result)) {
        return false;
    }

    if (!_database) {
        invariant(!intentLock);
        _database = dbHolder().openDb(txn, nss.ns());
    }
    CurOp::get(txn)->raiseDbProfileLevel(_database->getProfilingLevel());
    _collection = _database->getCollection(request->getTargetingNS());
    if (!_collection) {
        if (intentLock) {
            // try again with full X lock.
            unlock();
            return _lockAndCheckImpl(result, false);
        }

        MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
            WriteUnitOfWork wunit(txn);
            // Implicitly create if it doesn't exist
            _collection = _database->createCollection(txn, request->getTargetingNS());
            if (!_collection) {
                result->setError(toWriteError(
                    Status(ErrorCodes::InternalError,
                           "could not create collection " + request->getTargetingNS())));
                return false;
            }
            wunit.commit();
        }
        MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "createCollection", request->getTargetingNS());
    }
    return true;
}

bool WriteBatchExecutor::ExecInsertsState::lockAndCheck(WriteOpResult* result) {
    if (_lockAndCheckImpl(result, true))
        return true;
    unlock();
    return false;
}

void WriteBatchExecutor::ExecInsertsState::unlock() {
    _collection = nullptr;
    _database = nullptr;
    _collLock.reset();
    _dbLock.reset();
}

static void insertOne(WriteBatchExecutor::ExecInsertsState* state, WriteOpResult* result) {
    // we have to be top level so we can retry
    OperationContext* txn = state->txn;
    invariant(!txn->lockState()->inAWriteUnitOfWork());
    invariant(state->currIndex < state->normalizedInserts.size());

    const StatusWith<BSONObj>& normalizedInsert(state->normalizedInserts[state->currIndex]);

    if (!normalizedInsert.isOK()) {
        result->setError(toWriteError(normalizedInsert.getStatus()));
        return;
    }

    const BSONObj& insertDoc = normalizedInsert.getValue().isEmpty()
        ? state->request->getInsertRequest()->getDocumentsAt(state->currIndex)
        : normalizedInsert.getValue();

    try {
        MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
            if (state->request->isInsertIndexRequest()) {
                singleCreateIndex(txn, insertDoc, result);
            } else {
                if (state->lockAndCheck(result)) {
                    dassert(txn->lockState()->isCollectionLockedForMode(
                        state->getCollection()->ns().ns(), MODE_IX));

                    WriteUnitOfWork wunit(txn);
                    Status status = state->getCollection()->insertDocument(
                        txn, insertDoc, &CurOp::get(txn)->debug(), true);

                    if (status.isOK()) {
                        result->getStats().n++;
                        wunit.commit();
                    } else {
                        result->setError(toWriteError(status));
                    }
                }
            }
        }
        MONGO_WRITE_CONFLICT_RETRY_LOOP_END(
            txn, "insert", state->getCollection() ? state->getCollection()->ns().ns() : "index");
    } catch (const StaleConfigException& e) {
        result->setError(new WriteErrorDetail());
        buildStaleError(e.getVersionReceived(), e.getVersionWanted(), result->getError());
    } catch (const DBException& ex) {
        Status status(ex.toStatus());
        if (ErrorCodes::isInterruption(status.code()))
            throw;
        result->setError(toWriteError(status));
    }

    // Errors release the write lock, as a matter of policy.
    if (result->getError()) {
        txn->recoveryUnit()->abandonSnapshot();
        state->unlock();
    }
}

/**
 * Perform a single index creation on a collection.  Requires the index descriptor be
 * preprocessed.
 *
 * Might fault or error, otherwise populates the result.
 */
static void singleCreateIndex(OperationContext* txn,
                              const BSONObj& indexDesc,
                              WriteOpResult* result) {
    BSONElement nsElement = indexDesc["ns"];
    uassert(ErrorCodes::NoSuchKey, "Missing \"ns\" field in index description", !nsElement.eoo());
    uassert(ErrorCodes::TypeMismatch,
            str::stream() << "Expected \"ns\" field of index description to be a "
                             "string, "
                             "but found a " << typeName(nsElement.type()),
            nsElement.type() == String);
    const NamespaceString ns(nsElement.valueStringData());
    BSONObjBuilder cmdBuilder;
    cmdBuilder << "createIndexes" << ns.coll();
    cmdBuilder << "indexes" << BSON_ARRAY(indexDesc);
    BSONObj cmd = cmdBuilder.done();
    Command* createIndexesCmd = Command::findCommand("createIndexes");
    invariant(createIndexesCmd);
    std::string errmsg;
    BSONObjBuilder resultBuilder;
    const bool success =
        createIndexesCmd->run(txn, ns.db().toString(), cmd, 0, errmsg, resultBuilder);
    Command::appendCommandStatus(resultBuilder, success, errmsg);
    BSONObj cmdResult = resultBuilder.done();
    uassertStatusOK(getStatusFromCommandResult(cmdResult));
    result->getStats().n =
        cmdResult["numIndexesAfter"].numberInt() - cmdResult["numIndexesBefore"].numberInt();
}

static void multiUpdate(OperationContext* txn,
                        const BatchItemRef& updateItem,
                        WriteOpResult* result) {
    const NamespaceString nsString(updateItem.getRequest()->getNS());
    const bool isMulti = updateItem.getUpdate()->getMulti();
    UpdateRequest request(nsString);
    request.setQuery(updateItem.getUpdate()->getQuery());
    request.setUpdates(updateItem.getUpdate()->getUpdateExpr());
    request.setMulti(isMulti);
    request.setUpsert(updateItem.getUpdate()->getUpsert());
    UpdateLifecycleImpl updateLifecycle(true, request.getNamespaceString());
    request.setLifecycle(&updateLifecycle);

    // Updates from the write commands path can yield.
    request.setYieldPolicy(PlanExecutor::YIELD_AUTO);

    auto client = txn->getClient();
    auto lastOpAtOperationStart = repl::ReplClientInfo::forClient(client).getLastOp();
    ScopeGuard lastOpSetterGuard = MakeObjGuard(repl::ReplClientInfo::forClient(client),
                                                &repl::ReplClientInfo::setLastOpToSystemLastOpTime,
                                                txn);

    // If this is the local database, don't set last op.
    if (nsString.isLocal()) {
        lastOpSetterGuard.Dismiss();
    }

    int attempt = 0;
    bool createCollection = false;
    for (int fakeLoop = 0; fakeLoop < 1; fakeLoop++) {
        ParsedUpdate parsedUpdate(txn, &request);
        Status status = parsedUpdate.parseRequest();
        if (!status.isOK()) {
            result->setError(toWriteError(status));
            return;
        }

        if (createCollection) {
            MONGO_WRITE_CONFLICT_RETRY_LOOP_BEGIN {
                const AutoGetOrCreateDb adb{txn, nsString.db(), MODE_X};

                if (!checkIsMasterForDatabase(nsString, result)) {
                    return;
                }

                Database* const db = adb.getDb();
                if (db->getCollection(nsString.ns())) {
                    // someone else beat us to it
                } else {
                    WriteUnitOfWork wuow(txn);
                    uassertStatusOK(userCreateNS(txn, db, nsString.ns(), BSONObj()));
                    wuow.commit();
                }
            }
            MONGO_WRITE_CONFLICT_RETRY_LOOP_END(txn, "update", nsString.ns());
        }

        ///////////////////////////////////////////
        ScopedTransaction transaction(txn, MODE_IX);
        Lock::DBLock dbLock(txn->lockState(), nsString.db(), MODE_IX);
        Lock::CollectionLock colLock(
            txn->lockState(), nsString.ns(), parsedUpdate.isIsolated() ? MODE_X : MODE_IX);
        ///////////////////////////////////////////

        if (!checkIsMasterForDatabase(nsString, result)) {
            return;
        }

        if (!checkShardVersion(txn, *updateItem.getRequest(), result)) {
            return;
        }

        Database* const db = dbHolder().get(txn, nsString.db());

        if (db == NULL) {
            if (createCollection) {
                // we raced with some, accept defeat
                result->getStats().nModified = 0;
                result->getStats().n = 0;
                return;
            }

            // Database not yet created
            if (!request.isUpsert()) {
                // not an upsert, no database, nothing to do
                result->getStats().nModified = 0;
                result->getStats().n = 0;
                return;
            }

            //  upsert, don't try to get a context as no MODE_X lock is held
            fakeLoop = -1;
            createCollection = true;
            continue;
        }

        CurOp::get(txn)->raiseDbProfileLevel(db->getProfilingLevel());
        Collection* collection = db->getCollection(nsString.ns());

        if (collection == NULL) {
            if (createCollection) {
                // we raced with some, accept defeat
                result->getStats().nModified = 0;
                result->getStats().n = 0;
                return;
            }

            if (!request.isUpsert()) {
                // not an upsert, no collection, nothing to do
                result->getStats().nModified = 0;
                result->getStats().n = 0;
                return;
            }

            // upsert, mark that we should create collection
            fakeLoop = -1;
            createCollection = true;
            continue;
        }

        OpDebug* debug = &CurOp::get(txn)->debug();

        try {
            invariant(collection);
            std::unique_ptr<PlanExecutor> exec =
                uassertStatusOK(getExecutorUpdate(txn, debug, collection, &parsedUpdate));

            uassertStatusOK(exec->executePlan());

            PlanSummaryStats summary;
            Explain::getSummaryStats(*exec, &summary);
            collection->infoCache()->notifyOfQuery(txn, summary.indexesUsed);

            const UpdateStats* updateStats = UpdateStage::getUpdateStats(exec.get());
            UpdateStage::recordUpdateStatsInOpDebug(updateStats, debug);
            debug->setPlanSummaryMetrics(summary);

            UpdateResult res = UpdateStage::makeUpdateResult(updateStats);

            const long long numDocsModified = res.numDocsModified;
            const long long numMatched = res.numMatched;
            const BSONObj resUpsertedID = res.upserted;

            // We have an _id from an insert
            const bool didInsert = !resUpsertedID.isEmpty();

            result->getStats().nModified = numDocsModified;
            result->getStats().n = didInsert ? 1 : numMatched;
            result->getStats().upsertedID = resUpsertedID;

            if (repl::ReplClientInfo::forClient(client).getLastOp() != lastOpAtOperationStart) {
                // If this operation has already generated a new lastOp, don't bother setting it
                // here. No-op updates will not generate a new lastOp, so we still need the guard to
                // fire in that case.
                lastOpSetterGuard.Dismiss();
            }
        } catch (const WriteConflictException&) {
            debug->writeConflicts++;
            if (isMulti) {
                log() << "Had WriteConflict during multi update, aborting";
                throw;
            }

            createCollection = false;

            // RESTART LOOP
            fakeLoop = -1;
            txn->recoveryUnit()->abandonSnapshot();

            WriteConflictException::logAndBackoff(attempt++, "update", nsString.ns());
        } catch (const StaleConfigException& e) {
            result->setError(new WriteErrorDetail());
            buildStaleError(e.getVersionReceived(), e.getVersionWanted(), result->getError());
        } catch (const DBException& ex) {
            Status status = ex.toStatus();
            if (ErrorCodes::isInterruption(status.code())) {
                throw;
            }
            result->setError(toWriteError(status));
        }
    }
}

/**
 * Perform a remove operation, which might remove multiple documents.  Dispatches to remove code
 * currently to do most of this.
 *
 * Might fault or error, otherwise populates the result.
 */
static void multiRemove(OperationContext* txn,
                        const BatchItemRef& removeItem,
                        WriteOpResult* result) {
    const NamespaceString& nss = removeItem.getRequest()->getNS();
    DeleteRequest request(nss);
    request.setQuery(removeItem.getDelete()->getQuery());
    request.setMulti(removeItem.getDelete()->getLimit() != 1);
    request.setGod(false);

    // Deletes running through the write commands path can yield.
    request.setYieldPolicy(PlanExecutor::YIELD_AUTO);

    auto client = txn->getClient();
    auto lastOpAtOperationStart = repl::ReplClientInfo::forClient(client).getLastOp();
    ScopeGuard lastOpSetterGuard = MakeObjGuard(repl::ReplClientInfo::forClient(client),
                                                &repl::ReplClientInfo::setLastOpToSystemLastOpTime,
                                                txn);

    // If this is the local database, don't set last op.
    if (nss.isLocal()) {
        lastOpSetterGuard.Dismiss();
    }

    OpDebug* opDebug = &CurOp::get(txn)->debug();

    int attempt = 1;
    while (1) {
        try {
            ParsedDelete parsedDelete(txn, &request);
            Status status = parsedDelete.parseRequest();
            if (!status.isOK()) {
                result->setError(toWriteError(status));
                return;
            }

            ScopedTransaction scopedXact(txn, MODE_IX);
            AutoGetDb autoDb(txn, nss.db(), MODE_IX);
            if (!autoDb.getDb()) {
                break;
            }

            CurOp::get(txn)->raiseDbProfileLevel(autoDb.getDb()->getProfilingLevel());
            Lock::CollectionLock collLock(
                txn->lockState(), nss.ns(), parsedDelete.isIsolated() ? MODE_X : MODE_IX);

            // getExecutorDelete() also checks if writes are allowed.
            if (!checkIsMasterForDatabase(nss, result)) {
                return;
            }
            // Check version once we're locked

            if (!checkShardVersion(txn, *removeItem.getRequest(), result)) {
                // Version error
                return;
            }

            auto collection = autoDb.getDb()->getCollection(nss);

            std::unique_ptr<PlanExecutor> exec =
                uassertStatusOK(getExecutorDelete(txn, opDebug, collection, &parsedDelete));

            // Execute the delete and retrieve the number deleted.
            uassertStatusOK(exec->executePlan());
            result->getStats().n = DeleteStage::getNumDeleted(*exec);

            PlanSummaryStats summary;
            Explain::getSummaryStats(*exec, &summary);
            if (collection) {
                collection->infoCache()->notifyOfQuery(txn, summary.indexesUsed);
            }

            CurOp::get(txn)->debug().setPlanSummaryMetrics(summary);

            if (repl::ReplClientInfo::forClient(client).getLastOp() != lastOpAtOperationStart) {
                // If this operation has already generated a new lastOp, don't bother setting it
                // here. No-op updates will not generate a new lastOp, so we still need the guard to
                // fire in that case.
                lastOpSetterGuard.Dismiss();
            }
            break;
        } catch (const WriteConflictException&) {
            CurOp::get(txn)->debug().writeConflicts++;
            WriteConflictException::logAndBackoff(attempt++, "delete", nss.ns());
        } catch (const StaleConfigException& e) {
            result->setError(new WriteErrorDetail());
            buildStaleError(e.getVersionReceived(), e.getVersionWanted(), result->getError());
            return;
        } catch (const DBException& ex) {
            Status status = ex.toStatus();
            if (ErrorCodes::isInterruption(status.code())) {
                throw;
            }
            result->setError(toWriteError(status));
            return;
        }
    }
}

}  // namespace mongo
