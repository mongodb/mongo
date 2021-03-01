/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kSharding

#include "mongo/platform/basic.h"

#include "mongo/db/s/resharding/resharding_txn_cloner.h"

#include <vector>

#include "mongo/bson/bsonmisc.h"
#include "mongo/client/query.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/client.h"
#include "mongo/db/concurrency/d_concurrency.h"
#include "mongo/db/concurrency/write_conflict_exception.h"
#include "mongo/db/curop.h"
#include "mongo/db/logical_session_id.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/pipeline/sharded_agg_helpers.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/db/s/resharding/resharding_txn_cloner_progress_gen.h"
#include "mongo/db/s/session_catalog_migration_destination.h"
#include "mongo/db/session_catalog_mongod.h"
#include "mongo/db/session_txn_record_gen.h"
#include "mongo/db/transaction_participant.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/redaction.h"
#include "mongo/util/future_util.h"

namespace mongo {

ReshardingTxnCloner::ReshardingTxnCloner(ReshardingSourceId sourceId, Timestamp fetchTimestamp)
    : _sourceId(std::move(sourceId)), _fetchTimestamp(fetchTimestamp) {}

std::unique_ptr<Pipeline, PipelineDeleter> ReshardingTxnCloner::makePipeline(
    OperationContext* opCtx,
    std::shared_ptr<MongoProcessInterface> mongoProcessInterface,
    const boost::optional<LogicalSessionId>& startAfter) {
    const auto& sourceNss = NamespaceString::kSessionTransactionsTableNamespace;
    StringMap<ExpressionContext::ResolvedNamespace> resolvedNamespaces;
    resolvedNamespaces[sourceNss.coll()] = {sourceNss, std::vector<BSONObj>{}};

    auto expCtx = make_intrusive<ExpressionContext>(opCtx,
                                                    boost::none, /* explain */
                                                    false,       /* fromMongos */
                                                    false,       /* needsMerge */
                                                    false,       /* allowDiskUse */
                                                    false,       /* bypassDocumentValidation */
                                                    false,       /* isMapReduceCommand */
                                                    sourceNss,
                                                    boost::none, /* runtimeConstants */
                                                    nullptr,     /* collator */
                                                    std::move(mongoProcessInterface),
                                                    std::move(resolvedNamespaces),
                                                    boost::none /* collUUID */);

    Pipeline::SourceContainer stages;

    if (startAfter) {
        stages.emplace_back(DocumentSourceMatch::create(
            BSON(SessionTxnRecord::kSessionIdFieldName << BSON("$gt" << startAfter->toBSON())),
            expCtx));
    }

    stages.emplace_back(
        DocumentSourceSort::create(expCtx, BSON(SessionTxnRecord::kSessionIdFieldName << 1)));

    return Pipeline::create(std::move(stages), expCtx);
}

boost::optional<LogicalSessionId> ReshardingTxnCloner::_fetchProgressLsid(OperationContext* opCtx) {
    PersistentTaskStore<ReshardingTxnClonerProgress> store(
        NamespaceString::kReshardingTxnClonerProgressNamespace);

    boost::optional<LogicalSessionId> progressLsid;
    store.forEach(opCtx,
                  QUERY(ReshardingTxnClonerProgress::kSourceIdFieldName << _sourceId.toBSON()),
                  [&](const auto& doc) {
                      progressLsid = doc.getProgress();
                      return false;
                  });

    return progressLsid;
}

std::unique_ptr<Pipeline, PipelineDeleter> ReshardingTxnCloner::_targetAggregationRequest(
    OperationContext* opCtx, const Pipeline& pipeline) {
    AggregateCommand request(NamespaceString::kSessionTransactionsTableNamespace,
                             pipeline.serializeToBson());

    request.setReadConcern(BSON(repl::ReadConcernArgs::kLevelFieldName
                                << repl::readConcernLevels::kSnapshotName
                                << repl::ReadConcernArgs::kAtClusterTimeFieldName
                                << _fetchTimestamp));
    request.setWriteConcern(WriteConcernOptions());
    request.setHint(BSON(SessionTxnRecord::kSessionIdFieldName << 1));
    request.setUnwrappedReadPref(ReadPreferenceSetting{ReadPreference::Nearest}.toContainingBSON());

    return sharded_agg_helpers::runPipelineDirectlyOnSingleShard(
        pipeline.getContext(), std::move(request), _sourceId.getShardId());
}

template <typename Callable>
boost::optional<SharedSemiFuture<void>> ReshardingTxnCloner::_withSessionCheckedOut(
    OperationContext* opCtx, const SessionTxnRecord& donorRecord, Callable&& callable) {
    opCtx->setLogicalSessionId(donorRecord.getSessionId());
    opCtx->setTxnNumber(donorRecord.getTxnNum());

    MongoDOperationContextSession ocs(opCtx);
    auto txnParticipant = TransactionParticipant::get(opCtx);

    try {
        txnParticipant.beginOrContinue(opCtx, donorRecord.getTxnNum(), boost::none, boost::none);
    } catch (const DBException& ex) {
        if (ex.code() == ErrorCodes::TransactionTooOld) {
            // donorRecord.getTxnNum() < recipientTxnNumber
            return boost::none;
        } else if (ex.code() == ErrorCodes::IncompleteTransactionHistory) {
            // donorRecord.getTxnNum() == recipientTxnNumber &&
            // !txnParticipant.transactionIsInRetryableWriteMode()
            return boost::none;
        } else if (ex.code() == ErrorCodes::PreparedTransactionInProgress) {
            // txnParticipant.transactionIsPrepared()
            return txnParticipant.onExitPrepare();
        } else {
            throw;
        }
    }

    callable();
    return boost::none;
}

void ReshardingTxnCloner::_updateSessionRecord(OperationContext* opCtx) {
    invariant(opCtx->getLogicalSessionId());
    invariant(opCtx->getTxnNumber());

    repl::MutableOplogEntry oplogEntry;
    oplogEntry.setOpType(repl::OpTypeEnum::kNoop);
    oplogEntry.setObject(BSON(SessionCatalogMigrationDestination::kSessionMigrateOplogTag << 1));
    oplogEntry.setObject2(TransactionParticipant::kDeadEndSentinel);
    oplogEntry.setNss({});
    oplogEntry.setSessionId(opCtx->getLogicalSessionId());
    oplogEntry.setTxnNumber(opCtx->getTxnNumber());
    oplogEntry.setStatementId(kIncompleteHistoryStmtId);
    oplogEntry.setPrevWriteOpTimeInTransaction(repl::OpTime());
    oplogEntry.setWallClockTime(Date_t::now());
    oplogEntry.setFromMigrate(true);

    auto txnParticipant = TransactionParticipant::get(opCtx);
    writeConflictRetry(
        opCtx,
        "ReshardingTxnCloner::_updateSessionRecord",
        NamespaceString::kSessionTransactionsTableNamespace.ns(),
        [&] {
            // We need to take the global lock here so repl::logOp() will not unlock it and trigger
            // the invariant that disallows unlocking the global lock while inside a WUOW. We take
            // the transaction table's database lock to ensure the same lock ordering with normal
            // replicated updates to the collection.
            Lock::DBLock dbLock(
                opCtx, NamespaceString::kSessionTransactionsTableNamespace.db(), MODE_IX);

            WriteUnitOfWork wuow(opCtx);
            repl::OpTime opTime = repl::logOp(opCtx, &oplogEntry);

            uassert(4989901,
                    str::stream() << "Failed to create new oplog entry for oplog with opTime: "
                                  << oplogEntry.getOpTime().toString() << ": "
                                  << redact(oplogEntry.toBSON()),
                    !opTime.isNull());

            // Use the same wallTime as oplog since SessionUpdateTracker looks at the oplog entry
            // wallTime when replicating.
            SessionTxnRecord sessionTxnRecord(*opCtx->getLogicalSessionId(),
                                              *opCtx->getTxnNumber(),
                                              std::move(opTime),
                                              oplogEntry.getWallClockTime());

            txnParticipant.onRetryableWriteCloningCompleted(
                opCtx, {kIncompleteHistoryStmtId}, sessionTxnRecord);

            wuow.commit();
        });
}

void ReshardingTxnCloner::_updateProgressDocument(OperationContext* opCtx,
                                                  const LogicalSessionId& progress) {
    PersistentTaskStore<ReshardingTxnClonerProgress> store(
        NamespaceString::kReshardingTxnClonerProgressNamespace);

    store.upsert(
        opCtx,
        QUERY(ReshardingTxnClonerProgress::kSourceIdFieldName << _sourceId.toBSON()),
        BSON("$set" << BSON(ReshardingTxnClonerProgress::kProgressFieldName << progress.toBSON())),
        {1, WriteConcernOptions::SyncMode::UNSET, Seconds(0)});
}

/**
 * Invokes the 'callable' function with a fresh OperationContext.
 *
 * The OperationContext is configured so the RstlKillOpThread would always interrupt the operation
 * on step-up or stepdown, regardless of whether the operation has acquired any locks. This
 * interruption is best-effort to stop doing wasteful work on stepdown as quickly as possible. It
 * isn't required for the ReshardingTxnCloner's correctness. In particular, it is possible for an
 * OperationContext to be constructed after stepdown has finished, for the ReshardingTxnCloner to
 * run a getMore on the aggregation against the donor shards, and for the ReshardingTxnCloner to
 * only discover afterwards the recipient had already stepped down from a NotPrimary error when
 * updating a session record locally.
 *
 * Note that the recipient's primary-only service is responsible for managing the
 * ReshardingTxnCloner and would shut down the ReshardingTxnCloner's task executor following the
 * recipient stepping down.
 *
 * Also note that the ReshardingTxnCloner is only created after step-up as part of the recipient's
 * primary-only service and therefore would never be interrupted by step-up.
 */
template <typename Callable>
auto ReshardingTxnCloner::_withTemporaryOperationContext(Callable&& callable) {
    auto& client = cc();
    {
        stdx::lock_guard<Client> lk(client);
        invariant(client.canKillSystemOperationInStepdown(lk));
    }

    auto opCtx = client.makeOperationContext();
    opCtx->setAlwaysInterruptAtStepDownOrUp();

    // The BlockingResultsMerger underlying by the $mergeCursors stage records how long the
    // recipient spent waiting for documents from the donor shards. It doing so requires the CurOp
    // to be marked as having started.
    auto* curOp = CurOp::get(opCtx.get());
    curOp->ensureStarted();
    {
        ON_BLOCK_EXIT([curOp] { curOp->done(); });
        return callable(opCtx.get());
    }
}

ExecutorFuture<void> ReshardingTxnCloner::run(
    ServiceContext* serviceContext,
    std::shared_ptr<executor::TaskExecutor> executor,
    CancelationToken cancelToken,
    std::shared_ptr<MongoProcessInterface> mongoProcessInterface_forTest) {
    struct ChainContext {
        std::unique_ptr<Pipeline, PipelineDeleter> pipeline;
        boost::optional<SessionTxnRecord> donorRecord = boost::none;
        bool moreToCome = true;
        int progressCounter = 0;
    };

    auto chainCtx = std::make_shared<ChainContext>();

    return AsyncTry([this, chainCtx, mongoProcessInterface_forTest] {
               if (!chainCtx->pipeline) {
                   chainCtx->pipeline = _withTemporaryOperationContext([&](auto* opCtx) {
                       auto progressLsid = _fetchProgressLsid(opCtx);

                       auto mongoProcessInterface = MONGO_unlikely(mongoProcessInterface_forTest)
                           ? mongoProcessInterface_forTest
                           : MongoProcessInterface::create(opCtx);

                       auto pipeline = _targetAggregationRequest(
                           opCtx,
                           *makePipeline(opCtx, std::move(mongoProcessInterface), progressLsid));

                       pipeline->detachFromOperationContext();
                       pipeline.get_deleter().dismissDisposal();
                       return pipeline;
                   });

                   chainCtx->donorRecord = boost::none;
               }

               // A donor record will have been stashed on the ChainContext if we are resuming due
               // to a prepared transaction having been in progress.
               if (!chainCtx->donorRecord) {
                   chainCtx->donorRecord = _withTemporaryOperationContext([&](auto* opCtx) {
                       chainCtx->pipeline->reattachToOperationContext(opCtx);
                       auto doc = chainCtx->pipeline->getNext();
                       chainCtx->pipeline->detachFromOperationContext();

                       return doc ? SessionTxnRecord::parse(
                                        {"resharding config.transactions cloning"}, doc->toBson())
                                  : boost::optional<SessionTxnRecord>{};
                   });
               }

               if (!chainCtx->donorRecord) {
                   chainCtx->moreToCome = false;
                   return makeReadyFutureWith([] {}).share();
               }

               auto hitPreparedTxn = _withTemporaryOperationContext([&](auto* opCtx) {
                   return _withSessionCheckedOut(
                       opCtx, *chainCtx->donorRecord, [&] { _updateSessionRecord(opCtx); });
               });

               if (hitPreparedTxn) {
                   return *hitPreparedTxn;
               }

               chainCtx->progressCounter = (chainCtx->progressCounter + 1) %
                   resharding::gReshardingTxnClonerProgressBatchSize;

               if (chainCtx->progressCounter == 0) {
                   _withTemporaryOperationContext([&](auto* opCtx) {
                       _updateProgressDocument(opCtx, chainCtx->donorRecord->getSessionId());
                   });
               }

               chainCtx->donorRecord = boost::none;
               return makeReadyFutureWith([] {}).share();
           })
        .until([this, chainCtx](Status status) {
            if (status.isOK() && chainCtx->moreToCome) {
                return false;
            }

            if (chainCtx->pipeline) {
                _withTemporaryOperationContext([&](auto* opCtx) {
                    chainCtx->pipeline->dispose(opCtx);
                    chainCtx->pipeline.reset();
                });
            }

            if (status.isA<ErrorCategory::CancelationError>() ||
                status.isA<ErrorCategory::NotPrimaryError>()) {
                // Cancellation and NotPrimary errors indicate the primary-only service Instance
                // will be shut down or is shutting down now. Don't retry and leave resuming to when
                // the RecipientStateMachine is restarted on the new primary.
                return true;
            }

            if (status.isA<ErrorCategory::RetriableError>() ||
                status.isA<ErrorCategory::CursorInvalidatedError>() ||
                status == ErrorCodes::Interrupted) {
                // Do retry on any other types of retryable errors though. Also retry on errors from
                // stray killCursors and killOp commands being run.
                LOGV2(5461600,
                      "Transient error while cloning config.transactions collection",
                      "sourceId"_attr = _sourceId,
                      "fetchTimestamp"_attr = _fetchTimestamp,
                      "error"_attr = redact(status));
                return false;
            }

            if (!status.isOK()) {
                LOGV2(5461601,
                      "Operation-fatal error for resharding while cloning config.transactions"
                      " collection",
                      "sourceId"_attr = _sourceId,
                      "fetchTimestamp"_attr = _fetchTimestamp,
                      "error"_attr = redact(status));
            }

            return true;
        })
        .on(std::move(executor), std::move(cancelToken));
}

std::unique_ptr<Pipeline, PipelineDeleter> createConfigTxnCloningPipelineForResharding(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    Timestamp fetchTimestamp,
    boost::optional<LogicalSessionId> startAfter) {
    invariant(!fetchTimestamp.isNull());

    ReshardingSourceId sourceId(UUID::gen(), ShardId("dummyShardId"));
    ReshardingTxnCloner cloner(std::move(sourceId), fetchTimestamp);

    return cloner.makePipeline(expCtx->opCtx, expCtx->mongoProcessInterface, startAfter);
}

}  // namespace mongo
