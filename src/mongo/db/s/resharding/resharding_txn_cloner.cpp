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


#include "mongo/db/s/resharding/resharding_txn_cloner.h"

#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/db/client.h"
#include "mongo/db/curop.h"
#include "mongo/db/exec/agg/exec_pipeline.h"
#include "mongo/db/exec/agg/pipeline_builder.h"
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
#include "mongo/platform/atomic_word.h"
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
        false /* requestQueryStatsFromRemotes */);
}

std::unique_ptr<Pipeline> ReshardingTxnCloner::_restartPipeline(
    OperationContext* opCtx, std::shared_ptr<MongoProcessInterface> mongoProcessInterface) {
    auto progressLsid = _fetchProgressLsid(opCtx);
    auto pipeline = _targetAggregationRequest(
        opCtx, *makePipeline(opCtx, std::move(mongoProcessInterface), progressLsid));
    return pipeline;
}

boost::optional<SessionTxnRecord> ReshardingTxnCloner::_getNextRecord(
    OperationContext* opCtx, Pipeline& pipeline, exec::agg::Pipeline& execPipeline) {
    execPipeline.reattachToOperationContext(opCtx);
    pipeline.reattachToOperationContext(opCtx);
    ON_BLOCK_EXIT([&pipeline, &execPipeline] {
        execPipeline.detachFromOperationContext();
        pipeline.detachFromOperationContext();
    });

    // The BlockingResultsMerger underlying by the $mergeCursors stage records how long the
    // recipient spent waiting for documents from the donor shard. It doing so requires the CurOp to
    // be marked as having started.
    auto* curOp = CurOp::get(opCtx);
    curOp->ensureStarted();
    ON_BLOCK_EXIT([curOp] { curOp->done(); });

    auto doc = execPipeline.getNext();
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
    CancelableOperationContextFactory factory,
    std::shared_ptr<MongoProcessInterface> mongoProcessInterface_forTest) {
    struct ChainContext {
        std::unique_ptr<Pipeline> pipeline;
        std::unique_ptr<exec::agg::Pipeline> execPipeline;
        boost::optional<SessionTxnRecord> donorRecord;
        bool moreToCome = true;
        int progressCounter = 0;
    };

    auto chainCtx = std::make_shared<ChainContext>();

    return resharding::WithAutomaticRetry(
               [this, chainCtx, cancelToken, factory, mongoProcessInterface_forTest] {
                   if (!chainCtx->pipeline) {
                       auto opCtx = factory.makeOperationContext(&cc());
                       chainCtx->pipeline =
                           _restartPipeline(opCtx.get(),
                                            MONGO_unlikely(mongoProcessInterface_forTest)
                                                ? mongoProcessInterface_forTest
                                                : MongoProcessInterface::create(opCtx.get()));
                       chainCtx->execPipeline =
                           exec::agg::buildPipeline(chainCtx->pipeline->freeze());
                       chainCtx->execPipeline->dismissDisposal();
                       chainCtx->execPipeline->detachFromOperationContext();
                       chainCtx->pipeline->detachFromOperationContext();
                       chainCtx->donorRecord = boost::none;
                   }

                   // A donor record will have been stashed on the ChainContext if we are resuming
                   // due to a prepared transaction having been in progress.
                   if (!chainCtx->donorRecord) {
                       auto opCtx = factory.makeOperationContext(&cc());
                       ScopeGuard guard([&] {
                           chainCtx->execPipeline->reattachToOperationContext(opCtx.get());
                           chainCtx->execPipeline->dispose();
                           chainCtx->pipeline.reset();
                           chainCtx->execPipeline.reset();
                       });
                       chainCtx->donorRecord = _getNextRecord(
                           opCtx.get(), *chainCtx->pipeline, *chainCtx->execPipeline);
                       guard.dismiss();
                   }

                   if (!chainCtx->donorRecord) {
                       chainCtx->moreToCome = false;
                       return makeReadyFutureWith([] {}).semi();
                   }

                   {
                       auto opCtx = factory.makeOperationContext(&cc());
                       if (auto conflictingTxnCompletionFuture =
                               doOneRecord(opCtx.get(), *chainCtx->donorRecord)) {
                           return future_util::withCancellation(
                               std::move(*conflictingTxnCompletionFuture), cancelToken);
                       }
                   }

                   chainCtx->progressCounter = (chainCtx->progressCounter + 1) %
                       resharding::gReshardingTxnClonerProgressBatchSize.load();

                   if (chainCtx->progressCounter == 0) {
                       auto opCtx = factory.makeOperationContext(&cc());
                       _updateProgressDocument(opCtx.get(), chainCtx->donorRecord->getSessionId());
                   }

                   chainCtx->donorRecord = boost::none;
                   return makeReadyFutureWith([] {}).semi();
               })
        .onTransientError([this](const Status& status) {
            LOGV2(5461600,
                  "Transient error while cloning config.transactions collection",
                  "sourceId"_attr = _sourceId,
                  "readTimestamp"_attr = _fetchTimestamp,
                  "error"_attr = redact(status));
        })
        .onUnrecoverableError([this](const Status& status) {
            LOGV2_ERROR(
                5461601,
                "Operation-fatal error for resharding while cloning config.transactions collection",
                "sourceId"_attr = _sourceId,
                "readTimestamp"_attr = _fetchTimestamp,
                "error"_attr = redact(status));
        })
        .until<Status>([chainCtx, factory](const Status& status) {
            if (!status.isOK() && chainCtx->pipeline) {
                auto opCtx = factory.makeOperationContext(&cc());
                chainCtx->execPipeline->reattachToOperationContext(opCtx.get());
                chainCtx->execPipeline->dispose();
                chainCtx->pipeline.reset();
                chainCtx->execPipeline.reset();
            }

            return status.isOK() && !chainCtx->moreToCome;
        })
        .on(std::move(executor), cancelToken)
        .thenRunOn(std::move(cleanupExecutor))
        // It is unsafe to capture `this` once the task is running on the cleanupExecutor because
        // RecipientStateMachine, along with its ReshardingTxnCloner member, may have already been
        // destructed.
        .onCompletion([chainCtx](Status status) {
            if (chainCtx->pipeline) {
                // Guarantee the pipeline is always cleaned up - even upon cancellation.
                //
                // TODO(SERVER-111752): Please revisit if this thread could be made killable.
                auto client = cc().getServiceContext()
                                  ->getService(ClusterRole::ShardServer)
                                  ->makeClient("ReshardingTxnClonerCleanupClient",
                                               Client::noSession(),
                                               ClientOperationKillableByStepdown{false});

                AlternativeClientRegion acr(client);
                auto opCtx = cc().makeOperationContext();

                chainCtx->execPipeline->reattachToOperationContext(opCtx.get());
                chainCtx->execPipeline->dispose();
                chainCtx->pipeline.reset();
                chainCtx->execPipeline.reset();
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
