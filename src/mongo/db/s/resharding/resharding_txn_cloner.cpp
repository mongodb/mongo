// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0


#include "mongo/db/s/resharding/resharding_txn_cloner.h"

#include "mongo/base/status.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/exec/agg/exec_pipeline.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/persistent_task_store.h"
#include "mongo/db/pipeline/aggregate_command_gen.h"
#include "mongo/db/pipeline/document_source_match.h"
#include "mongo/db/pipeline/document_source_sort.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/pipeline/sharded_agg_helpers.h"
#include "mongo/db/repl/optime.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/s/resharding/resharding_data_copy_util.h"
#include "mongo/db/s/resharding/resharding_future_util.h"
#include "mongo/db/s/resharding/resharding_server_parameters_gen.h"
#include "mongo/db/s/resharding/resharding_txn_cloner_progress_gen.h"
#include "mongo/db/s/session_catalog_migration_util.h"
#include "mongo/db/service_context.h"
#include "mongo/db/session/logical_session_id.h"
#include "mongo/db/session/logical_session_id_helpers.h"
#include "mongo/db/session/session_txn_record_gen.h"
#include "mongo/db/sharding_environment/shard_id.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/idl/idl_parser.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/duration.h"
#include "mongo/util/future_impl.h"
#include "mongo/util/future_util.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/string_map.h"
#include "mongo/util/uuid.h"

#include <cstdint>
#include <mutex>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <absl/container/node_hash_map.h>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kResharding


namespace mongo {

ReshardingTxnCloner::ReshardingTxnCloner(ReshardingSourceId sourceId, Timestamp fetchTimestamp)
    : _sourceId(std::move(sourceId)), _fetchTimestamp(fetchTimestamp) {}

std::unique_ptr<Pipeline> ReshardingTxnCloner::makePipeline(
    OperationContext* opCtx,
    std::shared_ptr<MongoProcessInterface> mongoProcessInterface,
    const boost::optional<LogicalSessionId>& startAfter) {
    const auto& sourceNss = NamespaceString::kSessionTransactionsTableNamespace;
    ResolvedNamespaceMap resolvedNamespaces;
    resolvedNamespaces[sourceNss] = {sourceNss, std::vector<BSONObj>{}};
    auto expCtx = ExpressionContextBuilder{}
                      .opCtx(opCtx)
                      .mongoProcessInterface(std::move(mongoProcessInterface))
                      .ns(sourceNss)
                      .resolvedNamespace(std::move(resolvedNamespaces))
                      .build();

    DocumentSourceContainer stages;

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
                  BSON(ReshardingTxnClonerProgress::kSourceIdFieldName << _sourceId.toBSON()),
                  [&](const auto& doc) {
                      progressLsid = doc.getProgress();
                      return false;
                  });

    return progressLsid;
}

std::unique_ptr<Pipeline> ReshardingTxnCloner::_targetAggregationRequest(OperationContext* opCtx,
                                                                         const Pipeline& pipeline) {
    AggregateCommandRequest request(NamespaceString::kSessionTransactionsTableNamespace,
                                    pipeline.serializeToBson());

    request.setReadConcern(repl::ReadConcernArgs::snapshot(LogicalTime(_fetchTimestamp), true));
    request.setWriteConcern(WriteConcernOptions());
    request.setHint(BSON(SessionTxnRecord::kSessionIdFieldName << 1));
    request.setUnwrappedReadPref(ReadPreferenceSetting{ReadPreference::Nearest}.toContainingBSON());

    return sharded_agg_helpers::runPipelineDirectlyOnSingleShard(
        pipeline.getContext(),
        std::move(request),
        _sourceId.getShardId(),
        IncludeMetrics{} /* remoteMetricsToInclude */);
}

std::unique_ptr<Pipeline> ReshardingTxnCloner::_restartPipeline(
    OperationContext* opCtx, std::shared_ptr<MongoProcessInterface> mongoProcessInterface) {
    auto progressLsid = _fetchProgressLsid(opCtx);
    auto pipeline = _targetAggregationRequest(
        opCtx, *makePipeline(opCtx, std::move(mongoProcessInterface), progressLsid));
    return pipeline;
}

boost::optional<SessionTxnRecord> ReshardingTxnCloner::_getNextRecord(
    OperationContext* opCtx, resharding::ReshardingExecutablePipeline& pipeline) {
    pipeline.reattachToOpCtx(opCtx);
    ON_BLOCK_EXIT([&pipeline] { pipeline.detachFromOpCtx(); });

    // The BlockingResultsMerger underlying by the $mergeCursors stage records how long the
    // recipient spent waiting for documents from the donor shard. It doing so requires the CurOp to
    // be marked as having started.
    auto* curOp = CurOp::get(opCtx);
    curOp->ensureStarted();
    ON_BLOCK_EXIT([curOp] { curOp->done(); });

    auto doc = pipeline.get().getNext();
    return doc ? SessionTxnRecord::parse(doc->toBson(),
                                         IDLParserContext{"resharding config.transactions cloning"})
               : boost::optional<SessionTxnRecord>{};
}

boost::optional<SharedSemiFuture<void>> ReshardingTxnCloner::doOneRecord(
    OperationContext* opCtx, const SessionTxnRecord& donorRecord) {
    auto sessionId = donorRecord.getSessionId();
    auto txnNumber = donorRecord.getTxnNum();

    if (isInternalSessionForNonRetryableWrite(sessionId)) {
        // Skip internal sessions for non-retryable writes since they only support transactions
        // and those transactions are not retryable so there is no need to transfer the write
        // history to resharding recipient(s).
        return boost::none;
    }

    if (isInternalSessionForRetryableWrite(sessionId)) {
        // Turn this into write history for the retryable write that this internal transaction
        // corresponds to in order to avoid making retryable internal transactions have a sentinel
        // noop oplog entry at all.
        txnNumber = *sessionId.getTxnNumber();
        sessionId = *getParentSessionId(sessionId);
    }

    return session_catalog_migration_util::runWithSessionCheckedOutIfStatementNotExecuted(
        opCtx, sessionId, txnNumber, boost::none /* stmtId */, [&] {
            // If the TransactionParticipant is flagged as having an incomplete history, then the
            // dead end sentinel is already present in the oplog.
            if (TransactionParticipant::get(opCtx).hasIncompleteHistory()) {
                return;
            }

            resharding::data_copy::updateSessionRecord(opCtx,
                                                       TransactionParticipant::kDeadEndSentinel,
                                                       {kIncompleteHistoryStmtId},
                                                       boost::none /* preImageOpTime */,
                                                       boost::none /* postImageOpTime */,
                                                       {});
        });
}

void ReshardingTxnCloner::_updateProgressDocument(OperationContext* opCtx,
                                                  const LogicalSessionId& progress) {
    PersistentTaskStore<ReshardingTxnClonerProgress> store(
        NamespaceString::kReshardingTxnClonerProgressNamespace);

    store.upsert(
        opCtx,
        BSON(ReshardingTxnClonerProgress::kSourceIdFieldName << _sourceId.toBSON()),
        BSON("$set" << BSON(ReshardingTxnClonerProgress::kProgressFieldName << progress.toBSON())),
        WriteConcernOptions{1, WriteConcernOptions::SyncMode::UNSET, Seconds(0)});
}

SemiFuture<void> ReshardingTxnCloner::run(
    std::shared_ptr<executor::TaskExecutor> executor,
    std::shared_ptr<executor::TaskExecutor> cleanupExecutor,
    CancellationToken cancelToken,
    std::shared_ptr<HierarchicalCancelableOperationContextFactory> factory,
    std::shared_ptr<MongoProcessInterface> mongoProcessInterface_forTest) {
    struct ChainContext {
        resharding::ReshardingExecutablePipeline pipeline;
        boost::optional<SessionTxnRecord> donorRecord;
        bool moreToCome = true;
        int progressCounter = 0;
    };

    auto chainCtx = std::make_shared<ChainContext>();

    return resharding::WithAutomaticRetry(
               [this, chainCtx, cancelToken, factory, mongoProcessInterface_forTest] {
                   if (!chainCtx->pipeline.isInitialized()) {
                       auto opCtx = factory->makeOperationContext(&cc());
                       chainCtx->pipeline.reinitialize(
                           _restartPipeline(opCtx.get(),
                                            MONGO_unlikely(mongoProcessInterface_forTest)
                                                ? mongoProcessInterface_forTest
                                                : MongoProcessInterface::create(opCtx.get())));
                       chainCtx->pipeline.detachFromOpCtx();
                       chainCtx->donorRecord = boost::none;
                   }

                   // A donor record will have been stashed on the ChainContext if we are resuming
                   // due to a prepared transaction having been in progress.
                   if (!chainCtx->donorRecord) {
                       auto opCtx = factory->makeOperationContext(&cc());
                       ScopeGuard guard([&] { chainCtx->pipeline.dispose(opCtx.get()); });
                       chainCtx->donorRecord = _getNextRecord(opCtx.get(), chainCtx->pipeline);
                       guard.dismiss();
                   }

                   if (!chainCtx->donorRecord) {
                       chainCtx->moreToCome = false;
                       return makeReadyFutureWith([] {}).semi();
                   }

                   {
                       auto opCtx = factory->makeOperationContext(&cc());
                       if (auto conflictingTxnCompletionFuture =
                               doOneRecord(opCtx.get(), *chainCtx->donorRecord)) {
                           return future_util::withCancellation(
                               std::move(*conflictingTxnCompletionFuture), cancelToken);
                       }
                   }

                   chainCtx->progressCounter = (chainCtx->progressCounter + 1) %
                       resharding::gReshardingTxnClonerProgressBatchSize.load();

                   if (chainCtx->progressCounter == 0) {
                       auto opCtx = factory->makeOperationContext(&cc());
                       _updateProgressDocument(opCtx.get(), chainCtx->donorRecord->getSessionId());
                   }

                   chainCtx->donorRecord = boost::none;
                   return makeReadyFutureWith([] {}).semi();
               },
               // Treat ReplicaSetWritesBlocked as transient so config.transactions cloning holds
               // and resumes until the replica set write block is lifted, rather than failing the
               // operation, matching ReshardingCollectionCloner's handling. LockTimeout is left out
               // (retried by the recipient state machine that drives the cloner) so this only
               // extends the default retryability with the write block.
               resharding::kRetryabilityPredicateIncludeReplicaSetWritesBlockedAndWriteConcern)
        .onTransientError([this, chainCtx, factory](const Status& status) {
            if (status == ErrorCodes::ReplicaSetWritesBlocked) {
                if (resharding::shouldLogWriteBlockWarning(_lastWriteBlockWarningAt)) {
                    LOGV2_WARNING(10627300,
                                  "Resharding recipient is paused because writes to this replica "
                                  "set are currently blocked; config.transactions cloning will "
                                  "keep retrying until the write block is disabled or the "
                                  "operation is aborted",
                                  "sourceId"_attr = _sourceId,
                                  "error"_attr = redact(status));
                }
            } else {
                LOGV2(5461600,
                      "Transient error while cloning config.transactions collection",
                      "sourceId"_attr = _sourceId,
                      "readTimestamp"_attr = _fetchTimestamp,
                      "error"_attr = redact(status));
            }
            if (chainCtx->pipeline.isInitialized()) {
                auto opCtx = factory->makeOperationContext(&cc());
                chainCtx->pipeline.dispose(opCtx.get());
            }
        })
        .onUnrecoverableError([this](const Status& status) {
            LOGV2_ERROR(
                5461601,
                "Operation-fatal error for resharding while cloning config.transactions collection",
                "sourceId"_attr = _sourceId,
                "readTimestamp"_attr = _fetchTimestamp,
                "error"_attr = redact(status));
        })
        .untilRunOn(
            [chainCtx, factory] { return !chainCtx->moreToCome; }, std::move(executor), cancelToken)
        .thenRunOn(std::move(cleanupExecutor))
        // It is unsafe to capture `this` once the task is running on the cleanupExecutor because
        // RecipientStateMachine, along with its ReshardingTxnCloner member, may have already been
        // destructed.
        .onCompletion([chainCtx](Status status) {
            if (chainCtx->pipeline.isInitialized()) {
                // Guarantee the pipeline is always cleaned up - even upon cancellation.
                auto client = cc().getServiceContext()->getService()->makeClient(
                    "ReshardingTxnClonerCleanupClient", Client::noSession());

                AlternativeClientRegion acr(client);
                auto opCtx = cc().makeOperationContext();

                chainCtx->pipeline.dispose(opCtx.get());
            }

            // Propagate the result of the AsyncTry.
            return status;
        })
        .semi();
}

std::unique_ptr<Pipeline> createConfigTxnCloningPipelineForResharding(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    Timestamp fetchTimestamp,
    boost::optional<LogicalSessionId> startAfter) {
    invariant(!fetchTimestamp.isNull());

    ReshardingSourceId sourceId(UUID::gen(), ShardId("dummyShardId"));
    ReshardingTxnCloner cloner(std::move(sourceId), fetchTimestamp);

    return cloner.makePipeline(
        expCtx->getOperationContext(), expCtx->getMongoProcessInterface(), startAfter);
}

}  // namespace mongo
