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

    stages.emplace_back(DocumentSourceMatch::create(
        BSON((SessionTxnRecord::kLastWriteOpTimeFieldName + "." + repl::OpTime::kTimestampFieldName)
             << BSON("$lt" << _fetchTimestamp)),
        expCtx));

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
    AggregationRequest request(NamespaceString::kSessionTransactionsTableNamespace,
                               pipeline.serializeToBson());

    request.setReadConcern(BSON(repl::ReadConcernArgs::kLevelFieldName
                                << repl::readConcernLevels::kMajorityName
                                << repl::ReadConcernArgs::kAfterClusterTimeFieldName
                                << _fetchTimestamp));
    request.setWriteConcern({});
    request.setHint(BSON(SessionTxnRecord::kSessionIdFieldName << 1));
    request.setUnwrappedReadPref(ReadPreferenceSetting{ReadPreference::Nearest}.toContainingBSON());

    return sharded_agg_helpers::runPipelineDirectlyOnSingleShard(
        pipeline.getContext(), std::move(request), _sourceId.getShardId());
}

/**
 * Returns a fresh OperationContext.
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
ServiceContext::UniqueOperationContext ReshardingTxnCloner::_makeOperationContext(
    ServiceContext* serviceContext) {
    auto* client = Client::getCurrent();
    {
        stdx::lock_guard<Client> lk(*client);
        invariant(client->canKillSystemOperationInStepdown(lk));
    }

    auto opCtx = client->makeOperationContext();
    opCtx->setAlwaysInterruptAtStepDownOrUp();

    return opCtx;
}

ExecutorFuture<std::pair<ServiceContext::UniqueOperationContext,
                         std::unique_ptr<MongoDOperationContextSession>>>
ReshardingTxnCloner::_checkOutSession(ServiceContext* serviceContext,
                                      std::shared_ptr<executor::TaskExecutor> executor,
                                      SessionTxnRecord donorRecord) {
    auto opCtx = _makeOperationContext(serviceContext);
    opCtx->setLogicalSessionId(donorRecord.getSessionId());
    opCtx->setTxnNumber(donorRecord.getTxnNum());

    auto ocs = std::make_unique<MongoDOperationContextSession>(opCtx.get());
    auto txnParticipant = TransactionParticipant::get(opCtx.get());

    try {
        txnParticipant.beginOrContinue(
            opCtx.get(), donorRecord.getTxnNum(), boost::none, boost::none);
    } catch (const DBException& ex) {
        if (ex.code() == ErrorCodes::TransactionTooOld) {
            // donorRecord.getTxnNum() < recipientTxnNumber
            ocs.reset();
        } else if (ex.code() == ErrorCodes::IncompleteTransactionHistory) {
            // donorRecord.getTxnNum() == recipientTxnNumber &&
            // !txnParticipant.transactionIsInRetryableWriteMode()
            ocs.reset();
        } else if (ex.code() == ErrorCodes::PreparedTransactionInProgress) {
            // txnParticipant.transactionIsPrepared()
            return txnParticipant.onExitPrepare().thenRunOn(executor).then(
                [this, serviceContext, executor, donorRecord = std::move(donorRecord)] {
                    return _checkOutSession(
                        serviceContext, std::move(executor), std::move(donorRecord));
                });
        } else {
            throw;
        }
    }

    return ExecutorFuture(std::move(executor), std::make_pair(std::move(opCtx), std::move(ocs)));
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

            SessionTxnRecord sessionTxnRecord(*opCtx->getLogicalSessionId(),
                                              *opCtx->getTxnNumber(),
                                              std::move(opTime),
                                              Date_t::now());

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

template <typename Callable>
auto ReshardingTxnCloner::_withTemporaryOperationContext(ServiceContext* serviceContext,
                                                         Callable&& callable) {
    auto opCtx = _makeOperationContext(serviceContext);

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

ExecutorFuture<void> ReshardingTxnCloner::_updateSessionRecordsUntilPipelineExhausted(
    ServiceContext* serviceContext,
    std::shared_ptr<executor::TaskExecutor> executor,
    std::unique_ptr<Pipeline, PipelineDeleter> pipeline,
    int progressCounter) {
    auto doc = _withTemporaryOperationContext(serviceContext, [&](auto* opCtx) {
        pipeline->reattachToOperationContext(opCtx);
        auto doc = pipeline->getNext();
        pipeline->detachFromOperationContext();
        return doc;
    });

    if (!doc) {
        return ExecutorFuture(std::move(executor));
    }

    progressCounter = (progressCounter + 1) % resharding::gReshardingTxnClonerProgressBatchSize;

    auto donorRecord = SessionTxnRecord::parse(
        IDLParserErrorContext("resharding config.transactions cloning"), doc->toBson());

    return _checkOutSession(serviceContext, executor, std::move(donorRecord))
        .then([this](auto x) {
            const auto& [opCtx, ocs] = x;
            // _checkOutSession() doesn't leave the session checked out if its record shouldn't be
            // updated as a result of the donor's record.
            if (ocs) {
                _updateSessionRecord(opCtx.get());
            }
            return *opCtx->getLogicalSessionId();
        })
        .then([this, serviceContext, progressCounter](auto progressLsid) {
            if (progressCounter == 0) {
                _withTemporaryOperationContext(serviceContext, [&](auto* opCtx) {
                    _updateProgressDocument(opCtx, progressLsid);
                });
            }
        })
        .then([this,
               serviceContext,
               executor,
               pipeline = std::move(pipeline),
               progressCounter]() mutable {
            return _updateSessionRecordsUntilPipelineExhausted(
                serviceContext, std::move(executor), std::move(pipeline), progressCounter);
        });
}

ExecutorFuture<void> ReshardingTxnCloner::run(
    ServiceContext* serviceContext,
    std::shared_ptr<executor::TaskExecutor> executor,
    std::shared_ptr<MongoProcessInterface> mongoProcessInterface_forTest) {
    return ExecutorFuture(executor)
        .then([this, serviceContext] {
            return _withTemporaryOperationContext(
                serviceContext, [&](auto* opCtx) { return _fetchProgressLsid(opCtx); });
        })
        .then([this, serviceContext, mongoProcessInterface_forTest](auto progressLsid) {
            return _withTemporaryOperationContext(serviceContext, [&](auto* opCtx) {
                auto mongoProcessInterface = MONGO_unlikely(mongoProcessInterface_forTest)
                    ? std::move(mongoProcessInterface_forTest)
                    : MongoProcessInterface::create(opCtx);

                auto pipeline = _targetAggregationRequest(
                    opCtx, *makePipeline(opCtx, std::move(mongoProcessInterface), progressLsid));

                pipeline->detachFromOperationContext();
                return pipeline;
            });
        })
        .then([this, serviceContext, executor](auto pipeline) {
            return _updateSessionRecordsUntilPipelineExhausted(
                serviceContext, std::move(executor), std::move(pipeline), 0);
        });
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
