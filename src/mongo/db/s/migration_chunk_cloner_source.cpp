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

#include "mongo/db/s/migration_chunk_cloner_source.h"

#include <fmt/format.h>

#include "mongo/base/status.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/catalog/index_catalog.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/dbdirectclient.h"
#include "mongo/db/dbhelpers.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/index/index_access_method.h"
#include "mongo/db/index/index_descriptor.h"
#include "mongo/db/ops/write_ops_retryability.h"
#include "mongo/db/query/get_executor.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/replication_process.h"
#include "mongo/db/s/collection_sharding_runtime.h"
#include "mongo/db/s/migration_source_manager.h"
#include "mongo/db/s/operation_sharding_state.h"
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
#include "mongo/stdx/mutex.h"
#include "mongo/util/elapsed_tracker.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/str.h"
#include "mongo/util/time_support.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

namespace mongo {
namespace {

using namespace fmt::literals;

const char kRecvChunkStatus[] = "_recvChunkStatus";
const char kRecvChunkCommit[] = "_recvChunkCommit";
const char kRecvChunkAbort[] = "_recvChunkAbort";

const int kMaxObjectPerChunk{250000};
const Hours kMaxWaitToCommitCloneForJumboChunk(6);

MONGO_FAIL_POINT_DEFINE(failTooMuchMemoryUsed);

/**
 * Returns true if the given BSON object in the shard key value pair format is within the given
 * range.
 */
bool isShardKeyValueInRange(const BSONObj& shardKeyValue, const BSONObj& min, const BSONObj& max) {
    return shardKeyValue.woCompare(min) >= 0 && shardKeyValue.woCompare(max) < 0;
}

/**
 * Returns true if the given BSON document is within the given chunk range.
 */
bool isDocInRange(const BSONObj& obj,
                  const BSONObj& min,
                  const BSONObj& max,
                  const ShardKeyPattern& shardKeyPattern) {
    return isShardKeyValueInRange(shardKeyPattern.extractShardKeyFromDoc(obj), min, max);
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

BSONObj getDocumentKeyFromReplOperation(const repl::ReplOperation& replOperation) {
    switch (replOperation.getOpType()) {
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

std::vector<repl::ReplOperation> convertVector(const std::vector<repl::OplogEntry>& input) {
    std::vector<repl::ReplOperation> convertedOps;
    convertedOps.reserve(input.size());

    std::transform(
        input.cbegin(), input.cend(), std::back_inserter(convertedOps), [](const auto& op) {
            auto durableReplOp = op.getDurableReplOperation();
            if (!durableReplOp.isOwned()) {
                durableReplOp = repl::DurableReplOperation::parseOwned(
                    IDLParserContext{"MigrationChunkClonerSource_toOwnedDurableReplOperation"},
                    durableReplOp.toBSON());
            }

            return repl::ReplOperation(std::move(durableReplOp));
        });

    return convertedOps;
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
    LogOpForShardingHandler(MigrationChunkClonerSource* cloner,
                            const BSONObj& idObj,
                            const char op,
                            const repl::OpTime& opTime)
        : _cloner(cloner), _idObj(idObj.getOwned()), _op(op), _opTime(opTime) {}

    void commit(OperationContext* opCtx, boost::optional<Timestamp>) override {
        _cloner->_addToTransferModsQueue(_idObj, _op, _opTime);
        _cloner->_decrementOutstandingOperationTrackRequests();
    }

    void rollback(OperationContext* opCtx) override {
        _cloner->_decrementOutstandingOperationTrackRequests();
    }

private:
    MigrationChunkClonerSource* const _cloner;
    const BSONObj _idObj;
    const char _op;
    const repl::OpTime _opTime;
};

LogTransactionOperationsForShardingHandler::LogTransactionOperationsForShardingHandler(
    LogicalSessionId lsid,
    const std::vector<repl::OplogEntry>& stmts,
    repl::OpTime prepareOrCommitOpTime)
    : LogTransactionOperationsForShardingHandler(
          std::move(lsid), convertVector(stmts), std::move(prepareOrCommitOpTime)) {}

LogTransactionOperationsForShardingHandler::LogTransactionOperationsForShardingHandler(
    LogicalSessionId lsid,
    const std::vector<repl::ReplOperation>& stmts,
    repl::OpTime prepareOrCommitOpTime)
    : _lsid(std::move(lsid)),
      _stmts(stmts),
      _prepareOrCommitOpTime(std::move(prepareOrCommitOpTime)) {}

void LogTransactionOperationsForShardingHandler::commit(OperationContext* opCtx,
                                                        boost::optional<Timestamp>) {
    std::set<NamespaceString> namespacesTouchedByTransaction;

    // Inform the session migration subsystem that a transaction has committed for the given
    // namespace.
    auto addToSessionMigrationOptimeQueueIfNeeded =
        [&namespacesTouchedByTransaction, lsid = _lsid](MigrationChunkClonerSource* const cloner,
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

        // TODO (SERVER-71444): Fix to be interruptible or document exception.
        UninterruptibleLockGuard noInterrupt(opCtx->lockState());  // NOLINT.
        const auto scopedCss =
            CollectionShardingRuntime::assertCollectionLockedAndAcquireShared(opCtx, nss);

        auto clonerPtr = MigrationSourceManager::getCurrentCloner(*scopedCss);
        if (!clonerPtr) {
            continue;
        }
        auto* const cloner = dynamic_cast<MigrationChunkClonerSource*>(clonerPtr.get());

        if (isWouldChangeOwningShardSentinelOplogEntry(stmt)) {
            addToSessionMigrationOptimeQueueIfNeeded(cloner, nss, _prepareOrCommitOpTime);
            continue;
        }

        auto preImageDocKey = getDocumentKeyFromReplOperation(stmt);

        auto idElement = preImageDocKey["_id"];
        if (idElement.eoo()) {
            LOGV2_WARNING(21994,
                          "Received a document without an _id and will ignore that document",
                          "documentKey"_attr = redact(preImageDocKey));
            continue;
        }

        if (opType == repl::OpTypeEnum::kUpdate) {
            auto const& shardKeyPattern = cloner->_shardKeyPattern;
            auto preImageShardKeyValues =
                shardKeyPattern.extractShardKeyFromDocumentKey(preImageDocKey);

            // If prepare was performed from another term, we will not have the post image doc key
            // since it is not persisted in the oplog.
            auto postImageDocKey = stmt.getPostImageDocumentKey();
            if (!postImageDocKey.isEmpty()) {
                if (!cloner->_processUpdateForXferMod(preImageDocKey, postImageDocKey)) {
                    // We don't need to add this op to session migration if neither post or pre
                    // image doc falls within the chunk range.
                    continue;
                }
            } else {
                // We can't perform reads here using the same recovery unit because the transaction
                // is already committed. We instead defer performing the reads when xferMods command
                // is called. Also allow this op to be added to session migration since we can't
                // tell whether post image doc will fall within the chunk range. If it turns out
                // both preImage and postImage doc don't fall into the chunk range, it is not wrong
                // for this op to be added to session migration, but it will result in wasted work
                // and unneccesary extra oplog storage on the destination.
                cloner->_deferProcessingForXferMod(preImageDocKey);
            }
        } else {
            cloner->_addToTransferModsQueue(idElement.wrap(), getOpCharForCrudOpType(opType), {});
        }

        addToSessionMigrationOptimeQueueIfNeeded(cloner, nss, _prepareOrCommitOpTime);
    }
}

MigrationChunkClonerSource::MigrationChunkClonerSource(OperationContext* opCtx,
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
      _forceJumbo(_args.getForceJumbo() != ForceJumbo::kDoNotForce) {
    auto const replCoord = repl::ReplicationCoordinator::get(opCtx);
    if (replCoord->getReplicationMode() == repl::ReplicationCoordinator::modeReplSet) {
        _sessionCatalogSource = std::make_unique<SessionCatalogMigrationSource>(
            opCtx, nss(), ChunkRange(getMin(), getMax()), _shardKeyPattern.getKeyPattern());
    }
}

MigrationChunkClonerSource::~MigrationChunkClonerSource() {
    invariant(_state == kDone);
}

Status MigrationChunkClonerSource::startClone(OperationContext* opCtx,
                                              const UUID& migrationId,
                                              const LogicalSessionId& lsid,
                                              TxnNumber txnNumber) {
    invariant(_state == kNew);
    invariant(!opCtx->lockState()->isLocked());

    if (_sessionCatalogSource) {
        _sessionCatalogSource->init(opCtx);

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

Status MigrationChunkClonerSource::awaitUntilCriticalSectionIsAppropriate(
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

StatusWith<BSONObj> MigrationChunkClonerSource::commitClone(OperationContext* opCtx) {
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
            invariant(!_cloneList.hasMore());
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

void MigrationChunkClonerSource::cancelClone(OperationContext* opCtx) noexcept {
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

bool MigrationChunkClonerSource::isDocumentInMigratingChunk(const BSONObj& doc) {
    return isDocInRange(doc, getMin(), getMax(), _shardKeyPattern);
}

void MigrationChunkClonerSource::onInsertOp(OperationContext* opCtx,
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

    if (!isDocInRange(insertedDoc, getMin(), getMax(), _shardKeyPattern)) {
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

void MigrationChunkClonerSource::onUpdateOp(OperationContext* opCtx,
                                            boost::optional<BSONObj> preImageDoc,
                                            const BSONObj& postImageDoc,
                                            const repl::OpTime& opTime) {
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

    if (!isDocInRange(postImageDoc, getMin(), getMax(), _shardKeyPattern)) {
        // If the preImageDoc is not in range but the postImageDoc was, we know that the document
        // has changed shard keys and no longer belongs in the chunk being cloned. We will model
        // the deletion of the preImage document so that the destination chunk does not receive an
        // outdated version of this document.
        if (preImageDoc && isDocInRange(*preImageDoc, getMin(), getMax(), _shardKeyPattern)) {
            onDeleteOp(opCtx, *preImageDoc, opTime);
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

void MigrationChunkClonerSource::onDeleteOp(OperationContext* opCtx,
                                            const BSONObj& deletedDocId,
                                            const repl::OpTime& opTime) {
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

void MigrationChunkClonerSource::_addToSessionMigrationOptimeQueue(
    const repl::OpTime& opTime,
    SessionCatalogMigrationSource::EntryAtOpTimeType entryAtOpTimeType) {
    if (_sessionCatalogSource && !opTime.isNull()) {
        _sessionCatalogSource->notifyNewWriteOpTime(opTime, entryAtOpTimeType);
    }
}

void MigrationChunkClonerSource::_addToTransferModsQueue(const BSONObj& idObj,
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

bool MigrationChunkClonerSource::_addedOperationToOutstandingOperationTrackRequests() {
    stdx::unique_lock<Latch> lk(_mutex);
    if (!_acceptingNewOperationTrackRequests) {
        return false;
    }

    _incrementOutstandingOperationTrackRequests(lk);
    return true;
}

void MigrationChunkClonerSource::_drainAllOutstandingOperationTrackRequests(
    stdx::unique_lock<Latch>& lk) {
    invariant(_state == kDone);
    _acceptingNewOperationTrackRequests = false;
    _allOutstandingOperationTrackRequestsDrained.wait(
        lk, [&] { return _outstandingOperationTrackRequests == 0; });
}


void MigrationChunkClonerSource::_incrementOutstandingOperationTrackRequests(WithLock) {
    invariant(_acceptingNewOperationTrackRequests);
    ++_outstandingOperationTrackRequests;
}

void MigrationChunkClonerSource::_decrementOutstandingOperationTrackRequests() {
    stdx::lock_guard<Latch> sl(_mutex);
    --_outstandingOperationTrackRequests;
    if (_outstandingOperationTrackRequests == 0) {
        _allOutstandingOperationTrackRequestsDrained.notify_all();
    }
}

void MigrationChunkClonerSource::_nextCloneBatchFromIndexScan(OperationContext* opCtx,
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
            ShardingStatistics::get(opCtx).countBytesClonedOnDonor.addAndFetch(obj.objsize());
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

void MigrationChunkClonerSource::_nextCloneBatchFromCloneRecordIds(OperationContext* opCtx,
                                                                   const CollectionPtr& collection,
                                                                   BSONArrayBuilder* arrBuilder) {
    ElapsedTracker tracker(opCtx->getServiceContext()->getFastClockSource(),
                           internalQueryExecYieldIterations.load(),
                           Milliseconds(internalQueryExecYieldPeriodMS.load()));

    while (true) {
        int recordsNoLongerExist = 0;
        auto docInFlight = _cloneList.getNextDoc(opCtx, collection, &recordsNoLongerExist);

        if (recordsNoLongerExist) {
            stdx::lock_guard lk(_mutex);
            _numRecordsPassedOver += recordsNoLongerExist;
        }

        const auto& doc = docInFlight->getDoc();
        if (!doc) {
            break;
        }

        // We must always make progress in this method by at least one document because empty
        // return indicates there is no more initial clone data.
        if (arrBuilder->arrSize() && tracker.intervalHasElapsed()) {
            _cloneList.insertOverflowDoc(*doc);
            break;
        }

        // Do not send documents that are no longer in the chunk range being moved. This can
        // happen when document shard key value of the document changed after the initial
        // index scan during cloning. This is needed because the destination is very
        // conservative in processing xferMod deletes and won't delete docs that are not in
        // the range of the chunk being migrated.
        if (!isDocInRange(
                doc->value(), _args.getMin().value(), _args.getMax().value(), _shardKeyPattern)) {
            {
                stdx::lock_guard lk(_mutex);
                _numRecordsPassedOver++;
            }
            continue;
        }

        // Use the builder size instead of accumulating the document sizes directly so
        // that we take into consideration the overhead of BSONArray indices.
        if (arrBuilder->arrSize() &&
            (arrBuilder->len() + doc->value().objsize() + 1024) > BSONObjMaxUserSize) {
            _cloneList.insertOverflowDoc(*doc);
            break;
        }

        {
            stdx::lock_guard lk(_mutex);
            _numRecordsCloned++;
        }

        arrBuilder->append(doc->value());
        ShardingStatistics::get(opCtx).countDocsClonedOnDonor.addAndFetch(1);
        ShardingStatistics::get(opCtx).countBytesClonedOnDonor.addAndFetch(doc->value().objsize());
    }
}

uint64_t MigrationChunkClonerSource::getCloneBatchBufferAllocationSize() {
    stdx::lock_guard<Latch> sl(_mutex);
    if (_jumboChunkCloneState && _forceJumbo)
        return static_cast<uint64_t>(BSONObjMaxUserSize);

    return std::min(static_cast<uint64_t>(BSONObjMaxUserSize),
                    _averageObjectSizeForCloneRecordIds * _cloneList.size());
}

Status MigrationChunkClonerSource::nextCloneBatch(OperationContext* opCtx,
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

bool MigrationChunkClonerSource::_processUpdateForXferMod(const BSONObj& preImageDocKey,
                                                          const BSONObj& postImageDocKey) {
    auto const& minKey = _args.getMin().value();
    auto const& maxKey = _args.getMax().value();

    auto postShardKeyValues = _shardKeyPattern.extractShardKeyFromDocumentKey(postImageDocKey);
    fassert(6836100, !postShardKeyValues.isEmpty());

    auto opType = repl::OpTypeEnum::kUpdate;
    auto idElement = preImageDocKey["_id"];

    if (!isShardKeyValueInRange(postShardKeyValues, minKey, maxKey)) {
        // If the preImageDoc is not in range but the postImageDoc was, we know that the
        // document has changed shard keys and no longer belongs in the chunk being cloned.
        // We will model the deletion of the preImage document so that the destination chunk
        // does not receive an outdated version of this document.

        auto preImageShardKeyValues =
            _shardKeyPattern.extractShardKeyFromDocumentKey(preImageDocKey);
        fassert(6836101, !preImageShardKeyValues.isEmpty());

        if (!isShardKeyValueInRange(preImageShardKeyValues, minKey, maxKey)) {
            return false;
        }

        opType = repl::OpTypeEnum::kDelete;
        idElement = postImageDocKey["_id"];
    }

    _addToTransferModsQueue(idElement.wrap(), getOpCharForCrudOpType(opType), {});

    return true;
}

void MigrationChunkClonerSource::_deferProcessingForXferMod(const BSONObj& preImageDocKey) {
    stdx::lock_guard<Latch> sl(_mutex);
    _deferredReloadOrDeletePreImageDocKeys.push_back(preImageDocKey.getOwned());
    _deferredUntransferredOpsCounter++;
}

void MigrationChunkClonerSource::_processDeferredXferMods(OperationContext* opCtx) {
    std::vector<BSONObj> deferredReloadOrDeletePreImageDocKeys;

    {
        stdx::unique_lock lk(_mutex);
        deferredReloadOrDeletePreImageDocKeys.swap(_deferredReloadOrDeletePreImageDocKeys);
    }

    for (const auto& preImageDocKey : deferredReloadOrDeletePreImageDocKeys) {
        auto idElement = preImageDocKey["_id"];
        BSONObj newerVersionDoc;
        if (!Helpers::findById(opCtx, this->nss(), BSON("_id" << idElement), newerVersionDoc)) {
            // If the document can no longer be found, this means that another later op must have
            // deleted it. That delete would have been captured by the xferMods so nothing else to
            // do here.
            continue;
        }

        auto postImageDocKey =
            CollectionMetadata::extractDocumentKey(&_shardKeyPattern, newerVersionDoc);
        static_cast<void>(_processUpdateForXferMod(preImageDocKey, postImageDocKey));
    }
}

Status MigrationChunkClonerSource::nextModsBatch(OperationContext* opCtx, BSONObjBuilder* builder) {
    dassert(opCtx->lockState()->isCollectionLockedForMode(nss(), MODE_IS));

    _processDeferredXferMods(opCtx);

    std::list<BSONObj> deleteList;
    std::list<BSONObj> updateList;

    {
        // All clone data must have been drained before starting to fetch the incremental changes.
        stdx::unique_lock<Latch> lk(_mutex);
        invariant(!_cloneList.hasMore());

        // The "snapshot" for delete and update list must be taken under a single lock. This is to
        // ensure that we will preserve the causal order of writes. Always consume the delete
        // buffer first, before the update buffer. If the delete is causally before the update to
        // the same doc, then there's no problem since we consume the delete buffer first. If the
        // delete is causally after, we will not be able to see the document when we attempt to
        // fetch it, so it's also ok.
        deleteList.splice(deleteList.cbegin(), _deleted);
        updateList.splice(updateList.cbegin(), _reload);
    }

    BSONArrayBuilder arrDel(builder->subarrayStart("deleted"));
    auto noopFn = [](BSONObj idDoc, BSONObj* fullDoc) {
        *fullDoc = idDoc;
        return true;
    };
    long long totalDocSize = xferMods(&arrDel, &deleteList, 0, noopFn);
    arrDel.done();

    if (deleteList.empty()) {
        BSONArrayBuilder arrUpd(builder->subarrayStart("reload"));
        auto findByIdWrapper = [opCtx, this](BSONObj idDoc, BSONObj* fullDoc) {
            return Helpers::findById(opCtx, this->nss(), idDoc, *fullDoc);
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
    _deferredUntransferredOpsCounter = _deferredReloadOrDeletePreImageDocKeys.size();

    return Status::OK();
}

void MigrationChunkClonerSource::_cleanup() {
    stdx::unique_lock<Latch> lk(_mutex);
    _state = kDone;

    _drainAllOutstandingOperationTrackRequests(lk);

    _reload.clear();
    _untransferredUpsertsCounter = 0;
    _deleted.clear();
    _untransferredDeletesCounter = 0;
    _deferredReloadOrDeletePreImageDocKeys.clear();
    _deferredUntransferredOpsCounter = 0;
}

StatusWith<BSONObj> MigrationChunkClonerSource::_callRecipient(OperationContext* opCtx,
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
MigrationChunkClonerSource::_getIndexScanExecutor(OperationContext* opCtx,
                                                  const CollectionPtr& collection,
                                                  InternalPlanner::IndexScanOptions scanOption) {
    // Allow multiKey based on the invariant that shard keys must be single-valued. Therefore, any
    // multi-key index prefixed by shard key cannot be multikey over the shard key fields.
    const auto shardKeyIdx = findShardKeyPrefixedIndex(opCtx,
                                                       collection,
                                                       _shardKeyPattern.toBSON(),
                                                       /*requireSingleKey=*/false);
    if (!shardKeyIdx) {
        return {ErrorCodes::IndexNotFound,
                str::stream() << "can't find index with prefix " << _shardKeyPattern.toBSON()
                              << " in storeCurrentRecordId for " << nss().toStringForErrorMsg()};
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

Status MigrationChunkClonerSource::_storeCurrentRecordId(OperationContext* opCtx) {
    AutoGetCollection collection(opCtx, nss(), MODE_IS);
    if (!collection) {
        return {ErrorCodes::NamespaceNotFound,
                str::stream() << "Collection " << nss().toStringForErrorMsg()
                              << " does not exist."};
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
        RecordIdSet recordIdSet;

        while (PlanExecutor::ADVANCED == exec->getNext(&obj, &recordId)) {
            Status interruptStatus = opCtx->checkForInterruptNoAssert();
            if (!interruptStatus.isOK()) {
                return interruptStatus;
            }

            if (!isLargeChunk) {
                recordIdSet.insert(recordId);
            }

            if (++recCount > maxRecsWhenFull) {
                isLargeChunk = true;

                if (_forceJumbo) {
                    recordIdSet.clear();
                    break;
                }
            }
        }

        _cloneList.populateList(std::move(recordIdSet));
    } catch (DBException& exception) {
        exception.addContext("Executor error while scanning for documents belonging to chunk");
        throw;
    }

    const uint64_t collectionAverageObjectSize = collection->averageObjectSize(opCtx);

    uint64_t averageObjectIdSize = 0;
    const uint64_t defaultObjectIdSize = OID::kOIDSize;

    // For clustered collection, an index on '_id' is not required.
    if (totalRecs > 0 && !collection->isClustered()) {
        const auto idIdx = collection->getIndexCatalog()->findIdIndex(opCtx);
        if (!idIdx || !idIdx->getEntry()) {
            return {ErrorCodes::IndexNotFound,
                    str::stream() << "can't find index '_id' in storeCurrentRecordId for "
                                  << nss().toStringForErrorMsg()};
        }

        averageObjectIdSize =
            idIdx->getEntry()->accessMethod()->getSpaceUsedBytes(opCtx) / totalRecs;
    }

    if (isLargeChunk) {
        return {
            ErrorCodes::ChunkTooBig,
            str::stream() << "Cannot move chunk: the maximum number of documents for a chunk is "
                          << maxRecsWhenFull << ", the maximum chunk size is "
                          << _args.getMaxChunkSizeBytes() << ", average document size is "
                          << avgRecSize << ". Found " << recCount << " documents in chunk "
                          << " ns: " << nss().toStringForErrorMsg() << " " << getMin() << " -> "
                          << getMax()};
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

Status MigrationChunkClonerSource::_checkRecipientCloningStatus(OperationContext* opCtx,
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

        int64_t untransferredModsSizeBytes = _untransferredDeletesCounter * _averageObjectIdSize +
            (_untransferredUpsertsCounter + _deferredUntransferredOpsCounter) *
                _averageObjectSizeForCloneRecordIds;

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
                  "docsRemainingToClone"_attr =
                      _cloneList.size() - _numRecordsCloned - _numRecordsPassedOver,
                  "untransferredModsSizeBytes"_attr = untransferredModsSizeBytes);
        }

        if (res["state"].String() == "steady" && sessionCatalogSourceInCatchupPhase &&
            estimateUntransferredSessionsSize == 0) {
            if (_cloneList.hasMore() ||
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
                      "_deferredUntransferredOpsCounter"_attr = _deferredUntransferredOpsCounter,
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

boost::optional<repl::OpTime> MigrationChunkClonerSource::nextSessionMigrationBatch(
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
MigrationChunkClonerSource::getNotificationForNextSessionMigrationBatch() {
    if (!_sessionCatalogSource) {
        return nullptr;
    }

    return _sessionCatalogSource->getNotificationForNewOplog();
}

boost::optional<long long>
MigrationChunkClonerSource::getSessionOplogEntriesSkippedSoFarLowerBound() {
    if (!_sessionCatalogSource) {
        return boost::none;
    }

    return _sessionCatalogSource->getSessionOplogEntriesSkippedSoFarLowerBound();
}

boost::optional<long long> MigrationChunkClonerSource::getSessionOplogEntriesToBeMigratedSoFar() {
    if (!_sessionCatalogSource) {
        return boost::none;
    }

    return _sessionCatalogSource->getSessionOplogEntriesToBeMigratedSoFar();
}

MigrationChunkClonerSource::CloneList::DocumentInFlightWithLock::DocumentInFlightWithLock(
    WithLock lock, MigrationChunkClonerSource::CloneList& clonerList)
    : _inProgressReadToken(
          std::make_unique<MigrationChunkClonerSource::CloneList::InProgressReadToken>(
              lock, clonerList)) {}

void MigrationChunkClonerSource::CloneList::DocumentInFlightWithLock::setDoc(
    boost::optional<Snapshotted<BSONObj>> doc) {
    _doc = std::move(doc);
}

std::unique_ptr<MigrationChunkClonerSource::CloneList::DocumentInFlightWhileNotInLock>
MigrationChunkClonerSource::CloneList::DocumentInFlightWithLock::release() {
    invariant(_inProgressReadToken);

    return std::make_unique<MigrationChunkClonerSource::CloneList::DocumentInFlightWhileNotInLock>(
        std::move(_inProgressReadToken), std::move(_doc));
}

MigrationChunkClonerSource::CloneList::DocumentInFlightWhileNotInLock::
    DocumentInFlightWhileNotInLock(
        std::unique_ptr<CloneList::InProgressReadToken> inProgressReadToken,
        boost::optional<Snapshotted<BSONObj>> doc)
    : _inProgressReadToken(std::move(inProgressReadToken)), _doc(std::move(doc)) {}

void MigrationChunkClonerSource::CloneList::DocumentInFlightWhileNotInLock::setDoc(
    boost::optional<Snapshotted<BSONObj>> doc) {
    _doc = std::move(doc);
}

const boost::optional<Snapshotted<BSONObj>>&
MigrationChunkClonerSource::CloneList::DocumentInFlightWhileNotInLock::getDoc() {
    return _doc;
}

MigrationChunkClonerSource::CloneList::InProgressReadToken::InProgressReadToken(
    WithLock withLock, CloneList& cloneList)
    : _cloneList(cloneList) {
    _cloneList._startedOneInProgressRead(withLock);
}

MigrationChunkClonerSource::CloneList::InProgressReadToken::~InProgressReadToken() {
    _cloneList._finishedOneInProgressRead();
}

MigrationChunkClonerSource::CloneList::CloneList() {
    _recordIdsIter = _recordIds.begin();
}

void MigrationChunkClonerSource::CloneList::populateList(RecordIdSet recordIds) {
    stdx::lock_guard lk(_mutex);
    _recordIds = std::move(recordIds);
    _recordIdsIter = _recordIds.begin();
}

void MigrationChunkClonerSource::CloneList::insertOverflowDoc(Snapshotted<BSONObj> doc) {
    stdx::lock_guard lk(_mutex);
    invariant(_inProgressReads >= 1);
    _overflowDocs.push_back(std::move(doc));
}

bool MigrationChunkClonerSource::CloneList::hasMore() const {
    stdx::lock_guard lk(_mutex);
    return _recordIdsIter != _recordIds.cend() && _inProgressReads > 0;
}

std::unique_ptr<MigrationChunkClonerSource::CloneList::DocumentInFlightWhileNotInLock>
MigrationChunkClonerSource::CloneList::getNextDoc(OperationContext* opCtx,
                                                  const CollectionPtr& collection,
                                                  int* numRecordsNoLongerExist) {
    while (true) {
        stdx::unique_lock lk(_mutex);
        invariant(_inProgressReads >= 0);
        RecordId nextRecordId;

        opCtx->waitForConditionOrInterrupt(_moreDocsCV, lk, [&]() {
            return _recordIdsIter != _recordIds.end() || !_overflowDocs.empty() ||
                _inProgressReads == 0;
        });

        DocumentInFlightWithLock docInFlight(lk, *this);

        // One of the following must now be true (corresponding to the three if conditions):
        //   1.  There is a document in the overflow set
        //   2.  The iterator has not reached the end of the record id set
        //   3.  The overflow set is empty, the iterator is at the end, and
        //       no threads are holding a document.  This condition indicates
        //       that there are no more docs to return for the cloning phase.
        if (!_overflowDocs.empty()) {
            docInFlight.setDoc(std::move(_overflowDocs.front()));
            _overflowDocs.pop_front();
            return docInFlight.release();
        } else if (_recordIdsIter != _recordIds.end()) {
            nextRecordId = *_recordIdsIter;
            ++_recordIdsIter;
        } else {
            return docInFlight.release();
        }

        lk.unlock();

        auto docInFlightWhileNotLocked = docInFlight.release();

        Snapshotted<BSONObj> doc;
        if (collection->findDoc(opCtx, nextRecordId, &doc)) {
            docInFlightWhileNotLocked->setDoc(std::move(doc));
            return docInFlightWhileNotLocked;
        }

        if (numRecordsNoLongerExist) {
            (*numRecordsNoLongerExist)++;
        }
    }
}

size_t MigrationChunkClonerSource::CloneList::size() const {
    stdx::unique_lock lk(_mutex);
    return _recordIds.size();
}

void MigrationChunkClonerSource::CloneList::_startedOneInProgressRead(WithLock) {
    _inProgressReads++;
}

void MigrationChunkClonerSource::CloneList::_finishedOneInProgressRead() {
    stdx::lock_guard lk(_mutex);
    _inProgressReads--;
    _moreDocsCV.notify_one();
}

}  // namespace mongo
