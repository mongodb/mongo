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


#include "mongo/platform/basic.h"

#include "mongo/db/s/migration_chunk_cloner_source_legacy.h"

#include "mongo/base/status.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/ops/write_ops_retryability.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/migration_source_manager.h"
#include "mongo/db/s/shard_key_index_util.h"
#include "mongo/db/s/sharding_runtime_d_params_gen.h"
#include "mongo/db/s/sharding_statistics.h"
#include "mongo/db/s/start_chunk_clone_request.h"
#include "mongo/db/service_context.h"
#include "mongo/executor/remote_command_request.h"
#include "mongo/executor/remote_command_response.h"
#include "mongo/executor/task_executor.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/logv2/log.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/balancer_configuration.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/grid.h"
#include "mongo/util/elapsed_tracker.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding


namespace mongo {
namespace {

const char kRecvChunkStatus[] = "_recvChunkStatus";
const char kRecvChunkCommit[] = "_recvChunkCommit";
const char kRecvChunkAbort[] = "_recvChunkAbort";

const int kMaxObjectPerChunk{250000};
const Hours kMaxWaitToCommitCloneForJumboChunk(6);

MONGO_FAIL_POINT_DEFINE(failTooMuchMemoryUsed);

bool isInRange(const BSONObj& obj,
               const BSONObj& min,
               const BSONObj& max,
               const ShardKeyPattern& shardKeyPattern) {
    BSONObj k = shardKeyPattern.extractShardKeyFromDoc(obj);
    return k.woCompare(min) >= 0 && k.woCompare(max) < 0;
}

BSONObj createRequestWithSessionId(StringData commandName,
                                   const NamespaceString& nss,
                                   const MigrationSessionId& sessionId,
                                   bool waitForSteadyOrDone = false) {
    BSONObjBuilder builder;
    builder.append(commandName, nss.ns());
    builder.append("waitForSteadyOrDone", waitForSteadyOrDone);
    sessionId.append(&builder);
    return builder.obj();
}

BSONObj getDocumentKeyFromReplOperation(repl::ReplOperation replOperation,
                                        repl::OpTypeEnum opType) {
    switch (opType) {
        case repl::OpTypeEnum::kInsert:
        case repl::OpTypeEnum::kDelete:
            return replOperation.getObject();
        case repl::OpTypeEnum::kUpdate:
            return *replOperation.getObject2();
        default:
            MONGO_UNREACHABLE;
    }
    MONGO_UNREACHABLE;
}

char getOpCharForCrudOpType(repl::OpTypeEnum opType) {
    switch (opType) {
        case repl::OpTypeEnum::kInsert:
            return 'i';
        case repl::OpTypeEnum::kUpdate:
            return 'u';
        case repl::OpTypeEnum::kDelete:
            return 'd';
        default:
            MONGO_UNREACHABLE;
    }
    MONGO_UNREACHABLE;
}

}  // namespace

/**
 * Used to commit work for LogOpForSharding. Used to keep track of changes in documents that are
 * part of a chunk being migrated.
 */
class LogOpForShardingHandler final : public RecoveryUnit::Change {
public:
    /**
     * Invariant: idObj should belong to a document that is part of the active chunk being migrated
     */
    LogOpForShardingHandler(MigrationChunkClonerSourceLegacy* cloner,
                            const BSONObj& idObj,
                            const char op,
                            const repl::OpTime& opTime)
        : _cloner(cloner), _idObj(idObj.getOwned()), _op(op), _opTime(opTime) {}

    void commit(boost::optional<Timestamp>) override {
        _cloner->_addToTransferModsQueue(_idObj, _op, _opTime);
        _cloner->_decrementOutstandingOperationTrackRequests();
    }

    void rollback() override {
        _cloner->_decrementOutstandingOperationTrackRequests();
    }

private:
    MigrationChunkClonerSourceLegacy* const _cloner;
    const BSONObj _idObj;
    const char _op;
    const repl::OpTime _opTime;
};

void LogTransactionOperationsForShardingHandler::commit(boost::optional<Timestamp>) {
    std::set<NamespaceString> namespacesTouchedByTransaction;

    // Inform the session migration subsystem that a transaction has committed for the given
    // namespace.
    auto addToSessionMigrationOptimeQueueIfNeeded =
        [&namespacesTouchedByTransaction,
         lsid = _lsid](MigrationChunkClonerSourceLegacy* const cloner,
                       const NamespaceString& nss,
                       const repl::OpTime opTime) {
            if (isInternalSessionForNonRetryableWrite(lsid)) {
                // Transactions inside internal sessions for non-retryable writes are not
                // retryable so there is no need to transfer the write history to the
                // recipient.
                return;
            }
            if (namespacesTouchedByTransaction.find(nss) == namespacesTouchedByTransaction.end()) {
                cloner->_addToSessionMigrationOptimeQueue(
                    opTime, SessionCatalogMigrationSource::EntryAtOpTimeType::kTransaction);

                namespacesTouchedByTransaction.emplace(nss);
            }
        };

    for (const auto& stmt : _stmts) {
        auto opType = stmt.getOpType();

        // Skip every noop entry except for a WouldChangeOwningShard (WCOS) sentinel noop entry
        // since for an internal transaction for a retryable WCOS findAndModify that is an upsert,
        // the applyOps oplog entry on the old owning shard would not have the insert entry; so if
        // we skip the noop entry here, the write history for the internal transaction would not get
        // transferred to the recipient since the _prepareOrCommitOpTime would not get added to the
        // session migration opTime queue below, and this would cause the write to execute again if
        // there is a retry after the migration.
        if (opType == repl::OpTypeEnum::kNoop &&
            !isWouldChangeOwningShardSentinelOplogEntry(stmt)) {
            continue;
        }

        const auto& nss = stmt.getNss();
        auto opCtx = cc().getOperationContext();

        auto csr = CollectionShardingRuntime::get(opCtx, nss);
        UninterruptibleLockGuard noInterrupt(opCtx->lockState());
        auto csrLock = CollectionShardingRuntime::CSRLock::lockShared(opCtx, csr);

        const auto clonerPtr = MigrationSourceManager::getCurrentCloner(csr, csrLock);
        if (!clonerPtr) {
            continue;
        }
        auto* const cloner = dynamic_cast<MigrationChunkClonerSourceLegacy*>(clonerPtr.get());

        if (isWouldChangeOwningShardSentinelOplogEntry(stmt)) {
            addToSessionMigrationOptimeQueueIfNeeded(cloner, nss, _prepareOrCommitOpTime);
            continue;
        }

        auto documentKey = getDocumentKeyFromReplOperation(stmt, opType);

        auto idElement = documentKey["_id"];
        if (idElement.eoo()) {
            LOGV2_WARNING(21994,
                          "Received a document without an _id field, ignoring: {documentKey}",
                          "Received a document without an _id and will ignore that document",
                          "documentKey"_attr = redact(documentKey));
            continue;
        }

        auto const& minKey = cloner->_args.getMin().get();
        auto const& maxKey = cloner->_args.getMax().get();
        auto const& shardKeyPattern = cloner->_shardKeyPattern;

        if (!isInRange(documentKey, minKey, maxKey, shardKeyPattern)) {
            // If the preImageDoc is not in range but the postImageDoc was, we know that the
            // document has changed shard keys and no longer belongs in the chunk being cloned.
            // We will model the deletion of the preImage document so that the destination chunk
            // does not receive an outdated version of this document.
            if (opType == repl::OpTypeEnum::kUpdate &&
                isInRange(stmt.getPreImageDocumentKey(), minKey, maxKey, shardKeyPattern) &&
                !stmt.getPreImageDocumentKey()["_id"].eoo()) {
                opType = repl::OpTypeEnum::kDelete;
                idElement = stmt.getPreImageDocumentKey()["id"];
            } else {
                continue;
            }
        }

        addToSessionMigrationOptimeQueueIfNeeded(cloner, nss, _prepareOrCommitOpTime);

        cloner->_addToTransferModsQueue(idElement.wrap(), getOpCharForCrudOpType(opType), {});
    }
}

MigrationChunkClonerSourceLegacy::MigrationChunkClonerSourceLegacy(
    const ShardsvrMoveRange& request,
    const WriteConcernOptions& writeConcern,
    const BSONObj& shardKeyPattern,
    ConnectionString donorConnStr,
    HostAndPort recipientHost)
    : _args(request),
      _writeConcern(writeConcern),
      _shardKeyPattern(shardKeyPattern),
      _sessionId(MigrationSessionId::generate(_args.getFromShard().toString(),
                                              _args.getToShard().toString())),
      _donorConnStr(std::move(donorConnStr)),
      _recipientHost(std::move(recipientHost)),
      _forceJumbo(_args.getForceJumbo() != ForceJumbo::kDoNotForce) {}

MigrationChunkClonerSourceLegacy::~MigrationChunkClonerSourceLegacy() {
    invariant(_state == kDone);
}

Status MigrationChunkClonerSourceLegacy::startClone(OperationContext* opCtx,
                                                    const UUID& migrationId,
                                                    const LogicalSessionId& lsid,
                                                    TxnNumber txnNumber) {
    invariant(_state == kNew);
    invariant(!opCtx->lockState()->isLocked());

    auto const replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet) {
        _sessionCatalogSource = std::make_unique<SessionCatalogMigrationSource>(
            opCtx, nss(), ChunkRange(getMin(), getMax()), _shardKeyPattern.getKeyPattern());

        // Prime up the session migration source if there are oplog entries to migrate.
        _sessionCatalogSource->fetchNextOplog(opCtx);
    }

    {
        // Ignore prepare conflicts when we load ids of currently available documents. This is
        // acceptable because we will track changes made by prepared transactions at transaction
        // commit time.
        auto originalPrepareConflictBehavior = opCtx->recoveryUnit()->getPrepareConflictBehavior();

        ON_BLOCK_EXIT([&] {
            opCtx->recoveryUnit()->setPrepareConflictBehavior(originalPrepareConflictBehavior);
        });

        opCtx->recoveryUnit()->setPrepareConflictBehavior(
            PrepareConflictBehavior::kIgnoreConflicts);

        auto storeCurrentRecordIdStatus = _storeCurrentRecordId(opCtx);
        if (storeCurrentRecordIdStatus == ErrorCodes::ChunkTooBig && _forceJumbo) {
            stdx::lock_guard<Latch> sl(_mutex);
            _jumboChunkCloneState.emplace();
        } else if (!storeCurrentRecordIdStatus.isOK()) {
            return storeCurrentRecordIdStatus;
        }
    }

    // Tell the recipient shard to start cloning
    BSONObjBuilder cmdBuilder;

    const bool isThrottled = _args.getSecondaryThrottle();
    MigrationSecondaryThrottleOptions secondaryThrottleOptions = isThrottled
        ? MigrationSecondaryThrottleOptions::createWithWriteConcern(_writeConcern)
        : MigrationSecondaryThrottleOptions::create(MigrationSecondaryThrottleOptions::kOff);

    StartChunkCloneRequest::appendAsCommand(&cmdBuilder,
                                            nss(),
                                            migrationId,
                                            lsid,
                                            txnNumber,
                                            _sessionId,
                                            _donorConnStr,
                                            _args.getFromShard(),
                                            _args.getToShard(),
                                            getMin(),
                                            getMax(),
                                            _shardKeyPattern.toBSON(),
                                            secondaryThrottleOptions);

    // Commands sent to shards that accept writeConcern, must always have writeConcern. So if the
    // StartChunkCloneRequest didn't add writeConcern (from secondaryThrottle), then we add the
    // internal server default writeConcern.
    if (!cmdBuilder.hasField(WriteConcernOptions::kWriteConcernField)) {
        cmdBuilder.append(WriteConcernOptions::kWriteConcernField,
                          WriteConcernOptions::kInternalWriteDefault);
    }

    auto startChunkCloneResponseStatus = _callRecipient(opCtx, cmdBuilder.obj());
    if (!startChunkCloneResponseStatus.isOK()) {
        return startChunkCloneResponseStatus.getStatus();
    }

    // TODO (Kal): Setting the state to kCloning below means that if cancelClone was called we will
    // send a cancellation command to the recipient. The reason to limit the cases when we send
    // cancellation is for backwards compatibility with 3.2 nodes, which cannot differentiate
    // between cancellations for different migration sessions. It is thus possible that a second
    // migration from different donor, but the same recipient would certainly abort an already
    // running migration.
    stdx::lock_guard<Latch> sl(_mutex);
    _state = kCloning;

    return Status::OK();
}

Status MigrationChunkClonerSourceLegacy::awaitUntilCriticalSectionIsAppropriate(
    OperationContext* opCtx, Milliseconds maxTimeToWait) {
    invariant(_state == kCloning);
    invariant(!opCtx->lockState()->isLocked());
    // If this migration is manual migration that specified "force", enter the critical section
    // immediately. This means the entire cloning phase will be done under the critical section.
    if (_jumboChunkCloneState && _args.getForceJumbo() == ForceJumbo::kForceManual) {
        return Status::OK();
    }

    return _checkRecipientCloningStatus(opCtx, maxTimeToWait);
}

StatusWith<BSONObj> MigrationChunkClonerSourceLegacy::commitClone(OperationContext* opCtx) {
    invariant(_state == kCloning);
    invariant(!opCtx->lockState()->isLocked());
    if (_jumboChunkCloneState && _forceJumbo) {
        if (_args.getForceJumbo() == ForceJumbo::kForceManual) {
            auto status = _checkRecipientCloningStatus(opCtx, kMaxWaitToCommitCloneForJumboChunk);
            if (!status.isOK()) {
                return status;
            }
        } else {
            invariant(PlanExecutor::IS_EOF == _jumboChunkCloneState->clonerState);
            invariant(_cloneRecordIds.empty());
        }
    }

    if (_sessionCatalogSource) {
        _sessionCatalogSource->onCommitCloneStarted();
    }

    auto responseStatus = _callRecipient(opCtx, [&] {
        BSONObjBuilder builder;
        builder.append(kRecvChunkCommit, nss().ns());
        // For backward compatibility with v6.0 recipients.
        // TODO (SERVER-67844): Remove it once 7.0 becomes LTS.
        builder.append("acquireCSOnRecipient", true);
        _sessionId.append(&builder);
        return builder.obj();
    }());

    if (responseStatus.isOK()) {
        _cleanup();

        if (_sessionCatalogSource && _sessionCatalogSource->hasMoreOplog()) {
            return {ErrorCodes::SessionTransferIncomplete,
                    "destination shard finished committing but there are still some session "
                    "metadata that needs to be transferred"};
        }

        return responseStatus;
    }

    cancelClone(opCtx);
    return responseStatus.getStatus();
}

void MigrationChunkClonerSourceLegacy::cancelClone(OperationContext* opCtx) noexcept {
    invariant(!opCtx->lockState()->isLocked());

    if (_sessionCatalogSource) {
        _sessionCatalogSource->onCloneCleanup();
    }

    switch (_state) {
        case kDone:
            break;
        case kCloning: {
            const auto status =
                _callRecipient(opCtx,
                               createRequestWithSessionId(kRecvChunkAbort, nss(), _sessionId))
                    .getStatus();
            if (!status.isOK()) {
                LOGV2(21991,
                      "Failed to cancel migration: {error}",
                      "Failed to cancel migration",
                      "error"_attr = redact(status));
            }
            [[fallthrough]];
        }
        case kNew:
            _cleanup();
            break;
        default:
            MONGO_UNREACHABLE;
    }
}

bool MigrationChunkClonerSourceLegacy::isDocumentInMigratingChunk(const BSONObj& doc) {
    return isInRange(doc, getMin(), getMax(), _shardKeyPattern);
}

void MigrationChunkClonerSourceLegacy::onInsertOp(OperationContext* opCtx,
                                                  const BSONObj& insertedDoc,
                                                  const repl::OpTime& opTime) {
    dassert(opCtx->lockState()->isCollectionLockedForMode(nss(), MODE_IX));

    BSONElement idElement = insertedDoc["_id"];
    if (idElement.eoo()) {
        LOGV2_WARNING(21995,
                      "logInsertOp received a document without an _id field, ignoring inserted "
                      "document: {insertedDoc}",
                      "logInsertOp received a document without an _id field and will ignore that "
                      "document",
                      "insertedDoc"_attr = redact(insertedDoc));
        return;
    }

    if (!isInRange(insertedDoc, getMin(), getMax(), _shardKeyPattern)) {
        return;
    }

    if (!_addedOperationToOutstandingOperationTrackRequests()) {
        return;
    }

    if (opCtx->getTxnNumber()) {
        opCtx->recoveryUnit()->registerChange(
            std::make_unique<LogOpForShardingHandler>(this, idElement.wrap(), 'i', opTime));
    } else {
        opCtx->recoveryUnit()->registerChange(
            std::make_unique<LogOpForShardingHandler>(this, idElement.wrap(), 'i', repl::OpTime()));
    }
}

void MigrationChunkClonerSourceLegacy::onUpdateOp(OperationContext* opCtx,
                                                  boost::optional<BSONObj> preImageDoc,
                                                  const BSONObj& postImageDoc,
                                                  const repl::OpTime& opTime,
                                                  const repl::OpTime& prePostImageOpTime) {
    dassert(opCtx->lockState()->isCollectionLockedForMode(nss(), MODE_IX));

    BSONElement idElement = postImageDoc["_id"];
    if (idElement.eoo()) {
        LOGV2_WARNING(
            21996,
            "logUpdateOp received a document without an _id field, ignoring the updated document: "
            "{postImageDoc}",
            "logUpdateOp received a document without an _id field and will ignore that document",
            "postImageDoc"_attr = redact(postImageDoc));
        return;
    }

    if (!isInRange(postImageDoc, getMin(), getMax(), _shardKeyPattern)) {
        // If the preImageDoc is not in range but the postImageDoc was, we know that the document
        // has changed shard keys and no longer belongs in the chunk being cloned. We will model
        // the deletion of the preImage document so that the destination chunk does not receive an
        // outdated version of this document.
        if (preImageDoc && isInRange(*preImageDoc, getMin(), getMax(), _shardKeyPattern)) {
            onDeleteOp(opCtx, *preImageDoc, opTime, prePostImageOpTime);
        }
        return;
    }

    if (!_addedOperationToOutstandingOperationTrackRequests()) {
        return;
    }

    if (opCtx->getTxnNumber()) {
        opCtx->recoveryUnit()->registerChange(
            std::make_unique<LogOpForShardingHandler>(this, idElement.wrap(), 'u', opTime));
    } else {
        opCtx->recoveryUnit()->registerChange(
            std::make_unique<LogOpForShardingHandler>(this, idElement.wrap(), 'u', repl::OpTime()));
    }
}

void MigrationChunkClonerSourceLegacy::onDeleteOp(OperationContext* opCtx,
                                                  const BSONObj& deletedDocId,
                                                  const repl::OpTime& opTime,
                                                  const repl::OpTime&) {
    dassert(opCtx->lockState()->isCollectionLockedForMode(nss(), MODE_IX));

    BSONElement idElement = deletedDocId["_id"];
    if (idElement.eoo()) {
        LOGV2_WARNING(
            21997,
            "logDeleteOp received a document without an _id field, ignoring deleted doc: "
            "{deletedDocId}",
            "logDeleteOp received a document without an _id field and will ignore that document",
            "deletedDocId"_attr = redact(deletedDocId));
        return;
    }

    if (!_addedOperationToOutstandingOperationTrackRequests()) {
        return;
    }

    if (opCtx->getTxnNumber()) {
        opCtx->recoveryUnit()->registerChange(
            std::make_unique<LogOpForShardingHandler>(this, idElement.wrap(), 'd', opTime));
    } else {
        opCtx->recoveryUnit()->registerChange(
            std::make_unique<LogOpForShardingHandler>(this, idElement.wrap(), 'd', repl::OpTime()));
    }
}

void MigrationChunkClonerSourceLegacy::_addToSessionMigrationOptimeQueue(
    const repl::OpTime& opTime,
    SessionCatalogMigrationSource::EntryAtOpTimeType entryAtOpTimeType) {
    if (auto sessionSource = _sessionCatalogSource.get()) {
        if (!opTime.isNull()) {
            sessionSource->notifyNewWriteOpTime(opTime, entryAtOpTimeType);
        }
    }
}

void MigrationChunkClonerSourceLegacy::_addToTransferModsQueue(const BSONObj& idObj,
                                                               const char op,
                                                               const repl::OpTime& opTime) {
    switch (op) {
        case 'd': {
            stdx::lock_guard<Latch> sl(_mutex);
            _deleted.push_back(idObj);
            ++_untransferredDeletesCounter;
            _memoryUsed += idObj.firstElement().size() + 5;
        } break;

        case 'i':
        case 'u': {
            stdx::lock_guard<Latch> sl(_mutex);
            _reload.push_back(idObj);
            ++_untransferredUpsertsCounter;
            _memoryUsed += idObj.firstElement().size() + 5;
        } break;

        default:
            MONGO_UNREACHABLE;
    }

    _addToSessionMigrationOptimeQueue(
        opTime, SessionCatalogMigrationSource::EntryAtOpTimeType::kRetryableWrite);
}

bool MigrationChunkClonerSourceLegacy::_addedOperationToOutstandingOperationTrackRequests() {
    stdx::unique_lock<Latch> lk(_mutex);
    if (!_acceptingNewOperationTrackRequests) {
        return false;
    }

    _incrementOutstandingOperationTrackRequests(lk);
    return true;
}

void MigrationChunkClonerSourceLegacy::_drainAllOutstandingOperationTrackRequests(
    stdx::unique_lock<Latch>& lk) {
    invariant(_state == kDone);
    _acceptingNewOperationTrackRequests = false;
    _allOutstandingOperationTrackRequestsDrained.wait(
        lk, [&] { return _outstandingOperationTrackRequests == 0; });
}


void MigrationChunkClonerSourceLegacy::_incrementOutstandingOperationTrackRequests(WithLock) {
    invariant(_acceptingNewOperationTrackRequests);
    ++_outstandingOperationTrackRequests;
}

void MigrationChunkClonerSourceLegacy::_decrementOutstandingOperationTrackRequests() {
    stdx::lock_guard<Latch> sl(_mutex);
    --_outstandingOperationTrackRequests;
    if (_outstandingOperationTrackRequests == 0) {
        _allOutstandingOperationTrackRequestsDrained.notify_all();
    }
}

void MigrationChunkClonerSourceLegacy::_nextCloneBatchFromIndexScan(OperationContext* opCtx,
                                                                    const CollectionPtr& collection,
                                                                    BSONArrayBuilder* arrBuilder) {
    ElapsedTracker tracker(opCtx->getServiceContext()->getFastClockSource(),
                           internalQueryExecYieldIterations.load(),
                           Milliseconds(internalQueryExecYieldPeriodMS.load()));

    if (!_jumboChunkCloneState->clonerExec) {
        auto exec = uassertStatusOK(_getIndexScanExecutor(
            opCtx, collection, InternalPlanner::IndexScanOptions::IXSCAN_FETCH));
        _jumboChunkCloneState->clonerExec = std::move(exec);
    } else {
        _jumboChunkCloneState->clonerExec->reattachToOperationContext(opCtx);
        _jumboChunkCloneState->clonerExec->restoreState(&collection);
    }

    PlanExecutor::ExecState execState;
    try {
        BSONObj obj;
        RecordId recordId;
        while (PlanExecutor::ADVANCED ==
               (execState = _jumboChunkCloneState->clonerExec->getNext(&obj, nullptr))) {

            stdx::unique_lock<Latch> lk(_mutex);
            _jumboChunkCloneState->clonerState = execState;
            lk.unlock();

            opCtx->checkForInterrupt();

            // Use the builder size instead of accumulating the document sizes directly so
            // that we take into consideration the overhead of BSONArray indices.
            if (arrBuilder->arrSize() &&
                (arrBuilder->len() + obj.objsize() + 1024) > BSONObjMaxUserSize) {
                _jumboChunkCloneState->clonerExec->stashResult(obj);
                break;
            }

            arrBuilder->append(obj);

            lk.lock();
            _jumboChunkCloneState->docsCloned++;
            lk.unlock();

            ShardingStatistics::get(opCtx).countDocsClonedOnDonor.addAndFetch(1);
        }
    } catch (DBException& exception) {
        exception.addContext("Executor error while scanning for documents belonging to chunk");
        throw;
    }

    stdx::unique_lock<Latch> lk(_mutex);
    _jumboChunkCloneState->clonerState = execState;
    lk.unlock();

    _jumboChunkCloneState->clonerExec->saveState();
    _jumboChunkCloneState->clonerExec->detachFromOperationContext();
}

void MigrationChunkClonerSourceLegacy::_nextCloneBatchFromCloneRecordIds(
    OperationContext* opCtx, const CollectionPtr& collection, BSONArrayBuilder* arrBuilder) {
    ElapsedTracker tracker(opCtx->getServiceContext()->getFastClockSource(),
                           internalQueryExecYieldIterations.load(),
                           Milliseconds(internalQueryExecYieldPeriodMS.load()));

    stdx::unique_lock<Latch> lk(_mutex);
    auto iter = _cloneRecordIds.begin();

    for (; iter != _cloneRecordIds.end(); ++iter) {
        // We must always make progress in this method by at least one document because empty
        // return indicates there is no more initial clone data.
        if (arrBuilder->arrSize() && tracker.intervalHasElapsed()) {
            break;
        }

        auto nextRecordId = *iter;

        lk.unlock();

        Snapshotted<BSONObj> doc;
        if (collection->findDoc(opCtx, nextRecordId, &doc)) {
            // Use the builder size instead of accumulating the document sizes directly so
            // that we take into consideration the overhead of BSONArray indices.
            if (arrBuilder->arrSize() &&
                (arrBuilder->len() + doc.value().objsize() + 1024) > BSONObjMaxUserSize) {

                break;
            }

            arrBuilder->append(doc.value());
            ShardingStatistics::get(opCtx).countDocsClonedOnDonor.addAndFetch(1);
        }

        lk.lock();
    }

    _cloneRecordIds.erase(_cloneRecordIds.begin(), iter);
}

uint64_t MigrationChunkClonerSourceLegacy::getCloneBatchBufferAllocationSize() {
    stdx::lock_guard<Latch> sl(_mutex);
    if (_jumboChunkCloneState && _forceJumbo)
        return static_cast<uint64_t>(BSONObjMaxUserSize);

    return std::min(static_cast<uint64_t>(BSONObjMaxUserSize),
                    _averageObjectSizeForCloneRecordIds * _cloneRecordIds.size());
}

Status MigrationChunkClonerSourceLegacy::nextCloneBatch(OperationContext* opCtx,
                                                        const CollectionPtr& collection,
                                                        BSONArrayBuilder* arrBuilder) {
    dassert(opCtx->lockState()->isCollectionLockedForMode(nss(), MODE_IS));

    // If this chunk is too large to store records in _cloneRecordIds and the command args specify
    // to attempt to move it, scan the collection directly.
    if (_jumboChunkCloneState && _forceJumbo) {
        try {
            _nextCloneBatchFromIndexScan(opCtx, collection, arrBuilder);
            return Status::OK();
        } catch (const DBException& ex) {
            return ex.toStatus();
        }
    }

    _nextCloneBatchFromCloneRecordIds(opCtx, collection, arrBuilder);
    return Status::OK();
}

Status MigrationChunkClonerSourceLegacy::nextModsBatch(OperationContext* opCtx,
                                                       BSONObjBuilder* builder) {
    dassert(opCtx->lockState()->isCollectionLockedForMode(nss(), MODE_IS));

    std::list<BSONObj> deleteList;
    std::list<BSONObj> updateList;

    {
        // All clone data must have been drained before starting to fetch the incremental changes.
        stdx::unique_lock<Latch> lk(_mutex);
        invariant(_cloneRecordIds.empty());

        // The "snapshot" for delete and update list must be taken under a single lock. This is to
        // ensure that we will preserve the causal order of writes. Always consume the delete
        // buffer first, before the update buffer. If the delete is causally before the update to
        // the same doc, then there's no problem since we consume the delete buffer first. If the
        // delete is causally after, we will not be able to see the document when we attempt to
        // fetch it, so it's also ok.
        deleteList.splice(deleteList.cbegin(), _deleted);
        updateList.splice(updateList.cbegin(), _reload);
    }

    StringData ns = nss().ns().c_str();
    BSONArrayBuilder arrDel(builder->subarrayStart("deleted"));
    auto noopFn = [](BSONObj idDoc, BSONObj* fullDoc) {
        *fullDoc = idDoc;
        return true;
    };
    long long totalDocSize = xferMods(&arrDel, &deleteList, 0, noopFn);
    arrDel.done();

    if (deleteList.empty()) {
        BSONArrayBuilder arrUpd(builder->subarrayStart("reload"));
        auto findByIdWrapper = [opCtx, ns](BSONObj idDoc, BSONObj* fullDoc) {
            return Helpers::findById(opCtx, ns, idDoc, *fullDoc);
        };
        totalDocSize = xferMods(&arrUpd, &updateList, totalDocSize, findByIdWrapper);
        arrUpd.done();
    }

    builder->append("size", totalDocSize);

    // Put back remaining ids we didn't consume
    stdx::unique_lock<Latch> lk(_mutex);
    _deleted.splice(_deleted.cbegin(), deleteList);
    _untransferredDeletesCounter = _deleted.size();
    _reload.splice(_reload.cbegin(), updateList);
    _untransferredUpsertsCounter = _reload.size();

    return Status::OK();
}

void MigrationChunkClonerSourceLegacy::_cleanup() {
    stdx::unique_lock<Latch> lk(_mutex);
    _state = kDone;

    _drainAllOutstandingOperationTrackRequests(lk);

    _reload.clear();
    _untransferredUpsertsCounter = 0;
    _deleted.clear();
    _untransferredDeletesCounter = 0;
}

StatusWith<BSONObj> MigrationChunkClonerSourceLegacy::_callRecipient(OperationContext* opCtx,
                                                                     const BSONObj& cmdObj) {
    executor::RemoteCommandResponse responseStatus(
        Status{ErrorCodes::InternalError, "Uninitialized value"});

    auto executor = Grid::get(getGlobalServiceContext())->getExecutorPool()->getFixedExecutor();
    auto scheduleStatus = executor->scheduleRemoteCommand(
        executor::RemoteCommandRequest(_recipientHost, "admin", cmdObj, nullptr),
        [&responseStatus](const executor::TaskExecutor::RemoteCommandCallbackArgs& args) {
            responseStatus = args.response;
        });

    // TODO: Update RemoteCommandTargeter on NotWritablePrimary errors.
    if (!scheduleStatus.isOK()) {
        return scheduleStatus.getStatus();
    }

    auto cbHandle = scheduleStatus.getValue();

    try {
        executor->wait(cbHandle, opCtx);
    } catch (const DBException& ex) {
        // If waiting for the response is interrupted, then we still have a callback out and
        // registered with the TaskExecutor to run when the response finally does come back.
        // Since the callback references local state, cbResponse, it would be invalid for the
        // callback to run after leaving the this function. Therefore, we cancel the callback
        // and wait uninterruptably for the callback to be run.
        executor->cancel(cbHandle);
        executor->wait(cbHandle);
        return ex.toStatus();
    }

    if (!responseStatus.isOK()) {
        return responseStatus.status;
    }

    Status commandStatus = getStatusFromCommandResult(responseStatus.data);
    if (!commandStatus.isOK()) {
        return commandStatus;
    }

    return responseStatus.data.getOwned();
}

StatusWith<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>>
MigrationChunkClonerSourceLegacy::_getIndexScanExecutor(
    OperationContext* opCtx,
    const CollectionPtr& collection,
    InternalPlanner::IndexScanOptions scanOption) {
    // Allow multiKey based on the invariant that shard keys must be single-valued. Therefore, any
    // multi-key index prefixed by shard key cannot be multikey over the shard key fields.
    auto shardKeyIdx = findShardKeyPrefixedIndex(opCtx,
                                                 collection,
                                                 collection->getIndexCatalog(),
                                                 _shardKeyPattern.toBSON(),
                                                 /*requireSingleKey=*/false);
    if (!shardKeyIdx) {
        return {ErrorCodes::IndexNotFound,
                str::stream() << "can't find index with prefix " << _shardKeyPattern.toBSON()
                              << " in storeCurrentRecordId for " << nss().ns()};
    }

    // Assume both min and max non-empty, append MinKey's to make them fit chosen index
    const KeyPattern kp(shardKeyIdx->keyPattern());

    BSONObj min = Helpers::toKeyFormat(kp.extendRangeBound(getMin(), false));
    BSONObj max = Helpers::toKeyFormat(kp.extendRangeBound(getMax(), false));

    // We can afford to yield here because any change to the base data that we might miss is already
    // being queued and will migrate in the 'transferMods' stage.
    return InternalPlanner::shardKeyIndexScan(opCtx,
                                              &collection,
                                              *shardKeyIdx,
                                              min,
                                              max,
                                              BoundInclusion::kIncludeStartKeyOnly,
                                              PlanYieldPolicy::YieldPolicy::YIELD_AUTO,
                                              InternalPlanner::Direction::FORWARD,
                                              scanOption);
}

Status MigrationChunkClonerSourceLegacy::_storeCurrentRecordId(OperationContext* opCtx) {
    AutoGetCollection collection(opCtx, nss(), MODE_IS);
    if (!collection) {
        return {ErrorCodes::NamespaceNotFound,
                str::stream() << "Collection " << nss().ns() << " does not exist."};
    }

    auto swExec = _getIndexScanExecutor(
        opCtx, collection.getCollection(), InternalPlanner::IndexScanOptions::IXSCAN_DEFAULT);
    if (!swExec.isOK()) {
        return swExec.getStatus();
    }
    auto exec = std::move(swExec.getValue());

    // Use the average object size to estimate how many objects a full chunk would carry do that
    // while traversing the chunk's range using the sharding index, below there's a fair amount of
    // slack before we determine a chunk is too large because object sizes will vary.
    unsigned long long maxRecsWhenFull;
    long long avgRecSize;

    const long long totalRecs = collection->numRecords(opCtx);
    if (totalRecs > 0) {
        avgRecSize = collection->dataSize(opCtx) / totalRecs;
        // The calls to numRecords() and dataSize() are not atomic so it is possible that the data
        // size becomes smaller than the number of records between the two calls, which would result
        // in average record size of zero
        if (avgRecSize == 0) {
            avgRecSize = BSONObj::kMinBSONLength;
        }
        maxRecsWhenFull = std::max(_args.getMaxChunkSizeBytes() / avgRecSize, 1LL);
        maxRecsWhenFull = 2 * maxRecsWhenFull;  // pad some slack
    } else {
        avgRecSize = 0;
        maxRecsWhenFull = kMaxObjectPerChunk + 1;
    }

    // Do a full traversal of the chunk and don't stop even if we think it is a large chunk we want
    // the number of records to better report, in that case.
    bool isLargeChunk = false;
    unsigned long long recCount = 0;

    try {
        BSONObj obj;
        RecordId recordId;
        while (PlanExecutor::ADVANCED == exec->getNext(&obj, &recordId)) {
            Status interruptStatus = opCtx->checkForInterruptNoAssert();
            if (!interruptStatus.isOK()) {
                return interruptStatus;
            }

            if (!isLargeChunk) {
                stdx::lock_guard<Latch> lk(_mutex);
                _cloneRecordIds.insert(recordId);
            }

            if (++recCount > maxRecsWhenFull) {
                isLargeChunk = true;

                if (_forceJumbo) {
                    _cloneRecordIds.clear();
                    break;
                }
            }
        }
    } catch (DBException& exception) {
        exception.addContext("Executor error while scanning for documents belonging to chunk");
        throw;
    }

    const uint64_t collectionAverageObjectSize = collection->averageObjectSize(opCtx);

    uint64_t averageObjectIdSize = 0;
    const uint64_t defaultObjectIdSize = OID::kOIDSize;

    // For clustered collection, an index on '_id' is not required.
    if (totalRecs > 0 && !collection->isClustered()) {
        const auto idIdx = collection->getIndexCatalog()->findIdIndex(opCtx)->getEntry();
        if (!idIdx) {
            return {ErrorCodes::IndexNotFound,
                    str::stream() << "can't find index '_id' in storeCurrentRecordId for "
                                  << nss().ns()};
        }
        averageObjectIdSize = idIdx->accessMethod()->getSpaceUsedBytes(opCtx) / totalRecs;
    }

    if (isLargeChunk) {
        return {
            ErrorCodes::ChunkTooBig,
            str::stream() << "Cannot move chunk: the maximum number of documents for a chunk is "
                          << maxRecsWhenFull << ", the maximum chunk size is "
                          << _args.getMaxChunkSizeBytes() << ", average document size is "
                          << avgRecSize << ". Found " << recCount << " documents in chunk "
                          << " ns: " << nss().ns() << " " << getMin() << " -> " << getMax()};
    }

    stdx::lock_guard<Latch> lk(_mutex);
    _averageObjectSizeForCloneRecordIds = collectionAverageObjectSize + defaultObjectIdSize;
    _averageObjectIdSize = std::max(averageObjectIdSize, defaultObjectIdSize);
    return Status::OK();
}

long long xferMods(BSONArrayBuilder* arr,
                   std::list<BSONObj>* modsList,
                   long long initialSize,
                   std::function<bool(BSONObj, BSONObj*)> extractDocToAppendFn) {
    const long long maxSize = BSONObjMaxUserSize;

    if (modsList->empty() || initialSize > maxSize) {
        return initialSize;
    }

    stdx::unordered_set<absl::string_view> addedSet;
    auto iter = modsList->begin();
    for (; iter != modsList->end(); ++iter) {
        auto idDoc = *iter;
        absl::string_view idDocView(idDoc.objdata(), idDoc.objsize());

        if (addedSet.find(idDocView) == addedSet.end()) {
            addedSet.insert(idDocView);
            BSONObj fullDoc;
            if (extractDocToAppendFn(idDoc, &fullDoc)) {
                if (arr->arrSize() &&
                    (arr->len() + fullDoc.objsize() + kFixedCommandOverhead) > maxSize) {
                    break;
                }
                arr->append(fullDoc);
            }
        }
    }

    long long totalSize = arr->len();
    modsList->erase(modsList->begin(), iter);

    return totalSize;
}

Status MigrationChunkClonerSourceLegacy::_checkRecipientCloningStatus(OperationContext* opCtx,
                                                                      Milliseconds maxTimeToWait) {
    const auto startTime = Date_t::now();
    int iteration = 0;
    while ((Date_t::now() - startTime) < maxTimeToWait) {
        auto responseStatus = _callRecipient(
            opCtx, createRequestWithSessionId(kRecvChunkStatus, nss(), _sessionId, true));
        if (!responseStatus.isOK()) {
            return responseStatus.getStatus().withContext(
                "Failed to contact recipient shard to monitor data transfer");
        }

        const BSONObj& res = responseStatus.getValue();
        if (!res["waited"].boolean()) {
            sleepmillis(1LL << std::min(iteration, 10));
        }
        iteration++;

        const auto sessionCatalogSourceInCatchupPhase = _sessionCatalogSource->inCatchupPhase();
        const auto estimateUntransferredSessionsSize = sessionCatalogSourceInCatchupPhase
            ? _sessionCatalogSource->untransferredCatchUpDataSize()
            : std::numeric_limits<int64_t>::max();

        stdx::lock_guard<Latch> sl(_mutex);

        const std::size_t cloneRecordIdsRemaining = _cloneRecordIds.size();
        int64_t untransferredModsSizeBytes = _untransferredDeletesCounter * _averageObjectIdSize +
            _untransferredUpsertsCounter * _averageObjectSizeForCloneRecordIds;

        if (_forceJumbo && _jumboChunkCloneState) {
            LOGV2(21992,
                  "moveChunk data transfer progress: {response} mem used: {memoryUsedBytes} "
                  "documents cloned so far: {docsCloned} remaining amount: "
                  "{untransferredModsSizeBytes}",
                  "moveChunk data transfer progress",
                  "response"_attr = redact(res),
                  "memoryUsedBytes"_attr = _memoryUsed,
                  "docsCloned"_attr = _jumboChunkCloneState->docsCloned,
                  "untransferredModsSizeBytes"_attr = untransferredModsSizeBytes);
        } else {
            LOGV2(21993,
                  "moveChunk data transfer progress: {response} mem used: {memoryUsedBytes} "
                  "documents remaining to clone: {docsRemainingToClone} estimated remaining size "
                  "{untransferredModsSizeBytes}",
                  "moveChunk data transfer progress",
                  "response"_attr = redact(res),
                  "memoryUsedBytes"_attr = _memoryUsed,
                  "docsRemainingToClone"_attr = cloneRecordIdsRemaining,
                  "untransferredModsSizeBytes"_attr = untransferredModsSizeBytes);
        }

        if (res["state"].String() == "steady" && sessionCatalogSourceInCatchupPhase &&
            estimateUntransferredSessionsSize == 0) {
            if (cloneRecordIdsRemaining != 0 ||
                (_jumboChunkCloneState && _forceJumbo &&
                 PlanExecutor::IS_EOF != _jumboChunkCloneState->clonerState)) {
                return {ErrorCodes::OperationIncomplete,
                        str::stream() << "Unable to enter critical section because the recipient "
                                         "shard thinks all data is cloned while there are still "
                                         "documents remaining"};
            }

            return Status::OK();
        }

        bool supportsCriticalSectionDuringCatchUp = false;
        if (auto featureSupportedField =
                res[StartChunkCloneRequest::kSupportsCriticalSectionDuringCatchUp]) {
            if (!featureSupportedField.booleanSafe()) {
                return {ErrorCodes::Error(563070),
                        str::stream()
                            << "Illegal value for "
                            << StartChunkCloneRequest::kSupportsCriticalSectionDuringCatchUp};
            }
            supportsCriticalSectionDuringCatchUp = true;
        }

        if ((res["state"].String() == "steady" || res["state"].String() == "catchup") &&
            sessionCatalogSourceInCatchupPhase && supportsCriticalSectionDuringCatchUp) {
            auto estimatedUntransferredChunkPercentage =
                (std::min(_args.getMaxChunkSizeBytes(), untransferredModsSizeBytes) * 100) /
                _args.getMaxChunkSizeBytes();
            int64_t maxUntransferredSessionsSize = BSONObjMaxUserSize *
                _args.getMaxChunkSizeBytes() / ChunkSizeSettingsType::kDefaultMaxChunkSizeBytes;
            if (estimatedUntransferredChunkPercentage < maxCatchUpPercentageBeforeBlockingWrites &&
                estimateUntransferredSessionsSize < maxUntransferredSessionsSize) {
                // The recipient is sufficiently caught-up with the writes on the donor.
                // Block writes, so that it can drain everything.
                LOGV2(5630700,
                      "moveChunk data transfer within threshold to allow write blocking",
                      "_untransferredUpsertsCounter"_attr = _untransferredUpsertsCounter,
                      "_untransferredDeletesCounter"_attr = _untransferredDeletesCounter,
                      "_averageObjectSizeForCloneRecordIds"_attr =
                          _averageObjectSizeForCloneRecordIds,
                      "_averageObjectIdSize"_attr = _averageObjectIdSize,
                      "untransferredModsSizeBytes"_attr = untransferredModsSizeBytes,
                      "untransferredSessionDataInBytes"_attr = estimateUntransferredSessionsSize,
                      "maxChunksSizeBytes"_attr = _args.getMaxChunkSizeBytes(),
                      "_sessionId"_attr = _sessionId.toString());
                return Status::OK();
            }
        }

        if (res["state"].String() == "fail") {
            return {ErrorCodes::OperationFailed,
                    str::stream() << "Data transfer error: " << res["errmsg"].str()};
        }

        auto migrationSessionIdStatus = MigrationSessionId::extractFromBSON(res);
        if (!migrationSessionIdStatus.isOK()) {
            return {ErrorCodes::OperationIncomplete,
                    str::stream() << "Unable to retrieve the id of the migration session due to "
                                  << migrationSessionIdStatus.getStatus().toString()};
        }

        if (res["ns"].str() != nss().ns() ||
            (res.hasField("fromShardId")
                 ? (res["fromShardId"].str() != _args.getFromShard().toString())
                 : (res["from"].str() != _donorConnStr.toString())) ||
            !res["min"].isABSONObj() || res["min"].Obj().woCompare(getMin()) != 0 ||
            !res["max"].isABSONObj() || res["max"].Obj().woCompare(getMax()) != 0 ||
            !_sessionId.matches(migrationSessionIdStatus.getValue())) {
            // This can happen when the destination aborted the migration and received another
            // recvChunk before this thread sees the transition to the abort state. This is
            // currently possible only if multiple migrations are happening at once. This is an
            // unfortunate consequence of the shards not being able to keep track of multiple
            // incoming and outgoing migrations.
            return {ErrorCodes::OperationIncomplete,
                    "Destination shard aborted migration because a new one is running"};
        }

        if (_args.getForceJumbo() != ForceJumbo::kForceManual &&
            (_memoryUsed > 500 * 1024 * 1024 ||
             (_jumboChunkCloneState && MONGO_unlikely(failTooMuchMemoryUsed.shouldFail())))) {
            // This is too much memory for us to use so we're going to abort the migration
            return {ErrorCodes::ExceededMemoryLimit,
                    "Aborting migration because of high memory usage"};
        }

        Status interruptStatus = opCtx->checkForInterruptNoAssert();
        if (!interruptStatus.isOK()) {
            return interruptStatus;
        }
    }

    return {ErrorCodes::ExceededTimeLimit, "Timed out waiting for the cloner to catch up"};
}

boost::optional<repl::OpTime> MigrationChunkClonerSourceLegacy::nextSessionMigrationBatch(
    OperationContext* opCtx, BSONArrayBuilder* arrBuilder) {
    if (!_sessionCatalogSource) {
        return boost::none;
    }

    repl::OpTime opTimeToWaitIfWaitingForMajority;
    const ChunkRange range(getMin(), getMax());

    while (_sessionCatalogSource->hasMoreOplog()) {
        auto result = _sessionCatalogSource->getLastFetchedOplog();

        if (!result.oplog) {
            _sessionCatalogSource->fetchNextOplog(opCtx);
            continue;
        }

        auto newOpTime = result.oplog->getOpTime();
        auto oplogDoc = result.oplog->getEntry().toBSON();

        // Use the builder size instead of accumulating the document sizes directly so that we
        // take into consideration the overhead of BSONArray indices.
        if (arrBuilder->arrSize() &&
            (arrBuilder->len() + oplogDoc.objsize() + 1024) > BSONObjMaxUserSize) {
            break;
        }

        arrBuilder->append(oplogDoc);

        _sessionCatalogSource->fetchNextOplog(opCtx);

        if (result.shouldWaitForMajority) {
            if (opTimeToWaitIfWaitingForMajority < newOpTime) {
                opTimeToWaitIfWaitingForMajority = newOpTime;
            }
        }
    }

    return boost::make_optional(opTimeToWaitIfWaitingForMajority);
}

std::shared_ptr<Notification<bool>>
MigrationChunkClonerSourceLegacy::getNotificationForNextSessionMigrationBatch() {
    if (!_sessionCatalogSource) {
        return nullptr;
    }

    return _sessionCatalogSource->getNotificationForNewOplog();
}

}  // namespace mongo
