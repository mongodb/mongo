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

#include "mongo/db/commands/query_cmd/run_aggregate.h"

#include <absl/container/node_hash_map.h>
#include <absl/container/node_hash_set.h>
#include <boost/cstdint.hpp>
#include <boost/move/utility_core.hpp>
#include <boost/none.hpp>
#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>
#include <boost/smart_ptr.hpp>
#include <boost/smart_ptr/intrusive_ptr.hpp>
// IWYU pragma: no_include "ext/alloc_traits.h"
#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/client/read_preference.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/client.h"
#include "mongo/db/commands/query_cmd/aggregation_execution_state.h"
#include "mongo/db/curop.h"
#include "mongo/db/exec/agg/exchange_stage.h"
#include "mongo/db/exec/disk_use_options_gen.h"
#include "mongo/db/fle_crud.h"
#include "mongo/db/global_catalog/router_role_api/router_role.h"
#include "mongo/db/local_catalog/collection.h"
#include "mongo/db/local_catalog/collection_options.h"
#include "mongo/db/local_catalog/db_raii.h"
#include "mongo/db/local_catalog/external_data_source_scope_guard.h"
#include "mongo/db/local_catalog/shard_role_api/resource_yielders.h"
#include "mongo/db/local_catalog/shard_role_api/transaction_resources.h"
#include "mongo/db/local_catalog/shard_role_catalog/operation_sharding_state.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/memory_tracking/operation_memory_usage_tracker.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregation_hint_translation.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/pipeline/change_stream_invalidation_info.h"
#include "mongo/db/pipeline/document_source_exchange.h"
#include "mongo/db/pipeline/document_source_geo_near.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/expression_context_builder.h"
#include "mongo/db/pipeline/expression_context_diagnostic_printer.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/pipeline_d.h"
#include "mongo/db/pipeline/plan_executor_pipeline.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/db/pipeline/shard_role_transaction_resources_stasher_for_pipeline.h"
#include "mongo/db/pipeline/visitors/document_source_visitor_docs_needed_bounds.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/client_cursor/clientcursor.h"
#include "mongo/db/query/client_cursor/collect_query_stats_mongod.h"
#include "mongo/db/query/client_cursor/cursor_manager.h"
#include "mongo/db/query/client_cursor/cursor_response.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/collection_index_usage_tracker_decoration.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/explain_diagnostic_printer.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/plan_explainer.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/query/query_settings/query_settings_service.h"
#include "mongo/db/query/query_shape/agg_cmd_shape.h"
#include "mongo/db/query/query_stats/agg_key.h"
#include "mongo/db/query/query_stats/query_stats.h"
#include "mongo/db/query/shard_key_diagnostic_printer.h"
#include "mongo/db/raw_data_operation.h"
#include "mongo/db/read_concern.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/s/query_analysis_writer.h"
#include "mongo/db/server_feature_flags_gen.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/timeseries/timeseries_options.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/db/views/view.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/s/analyze_shard_key_common_gen.h"
#include "mongo/s/query_analysis_sampler_util.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/future.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/serialization_context.h"
#include "mongo/util/uuid.h"

#include <cstddef>
#include <functional>
#include <iosfwd>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

using boost::intrusive_ptr;
using std::string;
using std::stringstream;
using std::unique_ptr;
using NamespaceStringSet = stdx::unordered_set<NamespaceString>;

Counter64& allowDiskUseFalseCounter = *MetricBuilder<Counter64>{"query.allowDiskUseFalse"};
namespace {
// Ticks for server-side Javascript deprecation log messages.
Rarely _samplerAccumulatorJs, _samplerFunctionJs;

MONGO_FAIL_POINT_DEFINE(hangAfterCreatingAggregationPlan);

bool checkRetryableWriteAlreadyApplied(const AggExState& aggExState,
                                       rpc::ReplyBuilderInterface* result) {
    // The isRetryableWrite() check here is to check that the client executed write was
    // a retryable write (which would've spawned an internal session for a retryable write to
    // execute the two phase write without shard key protocol), otherwise we skip the retryable
    // write check.
    auto isClusterQueryWithoutShardKeyCmd =
        aggExState.getRequest().getIsClusterQueryWithoutShardKeyCmd();
    if (!aggExState.getOpCtx()->isRetryableWrite() || !isClusterQueryWithoutShardKeyCmd) {
        return false;
    }

    auto stmtId = aggExState.getRequest().getStmtId();
    tassert(7058100, "StmtId must be set for a retryable write without shard key", stmtId);
    bool wasWriteAlreadyApplied = TransactionParticipant::get(aggExState.getOpCtx())
                                      .checkStatementExecuted(aggExState.getOpCtx(), *stmtId);
    if (!wasWriteAlreadyApplied) {
        return false;
    }
    CursorResponseBuilder::Options options;
    options.isInitialResponse = true;
    CursorResponseBuilder responseBuilder(result, options);
    boost::optional<CursorMetrics> metrics = aggExState.getRequest().getIncludeQueryStatsMetrics()
        ? boost::make_optional(CursorMetrics{})
        : boost::none;
    responseBuilder.setWasStatementExecuted(true);
    responseBuilder.done(
        0LL,
        aggExState.getOriginalNss(),
        std::move(metrics),
        SerializationContext::stateCommandReply(aggExState.getRequest().getSerializationContext()));
    return true;
}

PlanExecutorPipeline::ResumableScanType getResumableScanType(const AggregateCommandRequest& request,
                                                             bool isChangeStream) {
    // $changeStream cannot be run on the oplog, and $_requestReshardingResumeToken can only be run
    // on the oplog. An aggregation request with both should therefore never reach this point.
    tassert(5353400,
            "$changeStream can't be combined with _requestReshardingResumeToken: true",
            !(isChangeStream && request.getRequestReshardingResumeToken()));
    if (isChangeStream) {
        return PlanExecutorPipeline::ResumableScanType::kChangeStream;
    }
    if (request.getRequestReshardingResumeToken()) {
        return PlanExecutorPipeline::ResumableScanType::kOplogScan;
    }
    if (request.getRequestResumeToken()) {
        return PlanExecutorPipeline::ResumableScanType::kNaturalOrderScan;
    }
    return PlanExecutorPipeline::ResumableScanType::kNone;
}

/**
 * If a pipeline is empty (assuming that a $cursor stage hasn't been created yet), it could mean
 * that we were able to absorb all pipeline stages and pull them into a single PlanExecutor. So,
 * instead of creating a whole pipeline to do nothing more than forward the results of its cursor
 * document source, we can optimize away the entire pipeline and answer the request using the query
 * engine only. This function checks if such optimization is possible.
 */
bool canOptimizeAwayPipeline(const AggExState& aggExState,
                             const Pipeline* pipeline,
                             const PlanExecutor* exec,
                             bool hasGeoNearStage) {
    return pipeline && exec && !hasGeoNearStage && !aggExState.hasChangeStream() &&
        pipeline->empty() &&
        // For exchange we will create a number of pipelines consisting of a single
        // DocumentSourceExchange stage, so cannot not optimize it away.
        !aggExState.getRequest().getExchange();
}

/**
 * Creates and registers a cursor with the global cursor manager. Returns the pinned cursor.
 */
ClientCursorPin registerCursor(const AggExState& aggExState,
                               const boost::intrusive_ptr<ExpressionContext>& expCtx,
                               unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec) {
    auto opCtx = aggExState.getOpCtx();
    ClientCursorParams cursorParams(
        std::move(exec),
        aggExState.getOriginalNss(),
        AuthorizationSession::get(opCtx->getClient())->getAuthenticatedUserName(),
        APIParameters::get(opCtx),
        opCtx->getWriteConcern(),
        repl::ReadConcernArgs::get(opCtx),
        ReadPreferenceSetting::get(opCtx),
        *aggExState.getDeferredCmd(),
        aggExState.getPrivileges());
    cursorParams.setTailableMode(expCtx->getTailableMode());

    // The global cursor manager does not deliver invalidations or kill notifications; the
    // underlying PlanExecutor(s) used by the pipeline will be receiving invalidations and kill
    // notifications themselves, not the cursor we create here.
    auto pin = CursorManager::get(opCtx)->registerCursor(opCtx, std::move(cursorParams));

    pin->incNBatches();
    auto extDataSrcGuard = aggExState.getExternalDataSourceScopeGuard();
    if (extDataSrcGuard) {
        ExternalDataSourceScopeGuard::get(pin.getCursor()) = extDataSrcGuard;
    }

    return pin;
}

/**
 * Updates query stats in OpDebug using the plan explainer from the pinned cursor (if given)
 * or the given executor (otherwise) and collects them in the query stats store.
 */
void collectQueryStats(const AggExState& aggExState,
                       const boost::intrusive_ptr<ExpressionContext>& expCtx,
                       mongo::PlanExecutor* maybeExec,
                       ClientCursorPin* maybePinnedCursor) {
    invariant(maybeExec || maybePinnedCursor);
    auto opCtx = aggExState.getOpCtx();
    auto curOp = CurOp::get(opCtx);
    const auto& planExplainer = maybePinnedCursor
        ? maybePinnedCursor->getCursor()->getExecutor()->getPlanExplainer()
        : maybeExec->getPlanExplainer();
    PlanSummaryStats stats;
    planExplainer.getSummaryStats(&stats);
    curOp->setEndOfOpMetrics(stats.nReturned);
    curOp->debug().setPlanSummaryMetrics(std::move(stats));

    if (maybePinnedCursor) {
        collectQueryStatsMongod(opCtx, *maybePinnedCursor);
    } else {
        collectQueryStatsMongod(opCtx, expCtx, std::move(curOp->debug().queryStatsInfo.key));
    }
}

/**
 * Builds the reply for a pipeline over a sharded collection that contains an exchange stage.
 */
void handleMultipleCursorsForExchange(const AggExState& aggExState,
                                      const boost::intrusive_ptr<ExpressionContext>& expCtx,
                                      std::vector<ClientCursorPin>& pinnedCursors,
                                      rpc::ReplyBuilderInterface* result) {
    invariant(pinnedCursors.size() > 1);
    collectQueryStats(aggExState, expCtx, nullptr, &pinnedCursors[0]);

    long long batchSize = aggExState.getRequest().getCursor().getBatchSize().value_or(
        aggregation_request_helper::kDefaultBatchSize);

    uassert(ErrorCodes::BadValue, "the exchange initial batch size must be zero", batchSize == 0);

    BSONArrayBuilder cursorsBuilder;
    for (auto&& pinnedCursor : pinnedCursors) {
        auto cursor = pinnedCursor.getCursor();
        invariant(cursor);

        BSONObjBuilder cursorResult;
        appendCursorResponseObject(cursor->cursorid(),
                                   aggExState.getOriginalNss(),
                                   BSONArray(),
                                   cursor->getExecutor()->getExecutorType(),
                                   &cursorResult,
                                   SerializationContext::stateCommandReply(
                                       aggExState.getRequest().getSerializationContext()));
        cursorResult.appendBool("ok", 1);

        cursorsBuilder.append(cursorResult.obj());

        // If a time limit was set on the pipeline, remaining time is "rolled over" to the cursor
        // (for use by future getmore ops).
        cursor->setLeftoverMaxTimeMicros(aggExState.getOpCtx()->getRemainingMaxTimeMicros());

        // Cursor needs to be in a saved state while we yield locks for getmore. State will be
        // restored in getMore().
        cursor->getExecutor()->saveState();
        cursor->getExecutor()->detachFromOperationContext();
    }

    auto bodyBuilder = result->getBodyBuilder();
    bodyBuilder.appendArray("cursors", cursorsBuilder.obj());
}

/**
 * Gets the first batch of documents produced by this pipeline by calling 'getNext()' on the
 * provided PlanExecutor. The provided CursorResponseBuilder will be populated with the batch.
 *
 * Returns true if we need to register a ClientCursor saved for this pipeline (for future getMore
 * requests). Otherwise, returns false.
 */
bool getFirstBatch(const AggExState& aggExState,
                   boost::intrusive_ptr<ExpressionContext> expCtx,
                   PlanExecutor& exec,
                   CursorResponseBuilder& responseBuilder) {
    // Capture diagnostics to be logged in the case of a failure.
    ScopedDebugInfo explainDiagnostics("explainDiagnostics",
                                       diagnostic_printers::ExplainDiagnosticPrinter{&exec});

    auto opCtx = aggExState.getOpCtx();
    long long batchSize = aggExState.getRequest().getCursor().getBatchSize().value_or(
        aggregation_request_helper::kDefaultBatchSize);

    auto curOp = CurOp::get(opCtx);

    bool doRegisterCursor = true;
    bool stashedResult = false;
    // We are careful to avoid ever calling 'getNext()' on the PlanExecutor when the batchSize is
    // zero to avoid doing any query execution work.
    for (int objCount = 0; objCount < batchSize; objCount++) {
        PlanExecutor::ExecState state;
        BSONObj nextDoc;

        try {
            state = exec.getNext(&nextDoc, nullptr);
        } catch (const ExceptionFor<ErrorCodes::CloseChangeStream>&) {
            // This exception is thrown when a $changeStream stage encounters an event that
            // invalidates the cursor. We should close the cursor and return without error.
            doRegisterCursor = false;
            break;
        } catch (const ExceptionFor<ErrorCodes::ChangeStreamInvalidated>& ex) {
            // This exception is thrown when a change-stream cursor is invalidated. Set the PBRT to
            // the resume token of the invalidating event, and mark the cursor response as
            // invalidated. We expect ExtraInfo to always be present for this exception.
            const auto extraInfo = ex.extraInfo<ChangeStreamInvalidationInfo>();
            tassert(5493701, "Missing ChangeStreamInvalidationInfo on exception", extraInfo);

            responseBuilder.setPostBatchResumeToken(extraInfo->getInvalidateResumeToken());
            responseBuilder.setInvalidated();

            doRegisterCursor = false;
            break;
        } catch (DBException& exception) {
            auto&& explainer = exec.getPlanExplainer();
            auto&& [stats, _] =
                explainer.getWinningPlanStats(ExplainOptions::Verbosity::kExecStats);
            LOGV2_WARNING(23799,
                          "Aggregate command executor error",
                          "error"_attr = exception.toStatus(),
                          "stats"_attr = redact(stats),
                          "cmd"_attr = redact(*aggExState.getDeferredCmd()));

            exception.addContext(str::stream()
                                 << "Executor error during aggregate command on namespace: "
                                 << aggExState.getOriginalNss().toStringForErrorMsg());
            throw;
        }

        if (state == PlanExecutor::IS_EOF) {
            // If this executor produces a postBatchResumeToken, add it to the cursor response. We
            // call this on EOF because the PBRT may advance even when there are no further results.
            responseBuilder.setPostBatchResumeToken(exec.getPostBatchResumeToken());

            if (!expCtx->isTailable()) {
                // There are no more documents, and the query is not tailable, so no need to create
                // a cursor.
                doRegisterCursor = false;
            }

            break;
        }

        invariant(state == PlanExecutor::ADVANCED);

        // If adding this object will cause us to exceed the message size limit, then we stash it
        // for later.
        if (!FindCommon::haveSpaceForNext(nextDoc, objCount, responseBuilder.bytesUsed())) {
            exec.stashResult(nextDoc);
            stashedResult = true;
            break;
        }

        // If this executor produces a postBatchResumeToken, add it to the cursor response.
        responseBuilder.setPostBatchResumeToken(exec.getPostBatchResumeToken());
        responseBuilder.append(nextDoc);
    }

    if (doRegisterCursor) {
        // For empty batches, or in the case where the final result was added to the batch rather
        // than being stashed, we update the PBRT to ensure that it is the most recent available.
        if (!stashedResult) {
            responseBuilder.setPostBatchResumeToken(exec.getPostBatchResumeToken());
        }

        // Cursor needs to be in a saved state while we yield locks for getmore. State will be
        // restored in getMore().
        exec.saveState();
        exec.detachFromOperationContext();
    } else {
        curOp->debug().cursorExhausted = true;
    }

    return doRegisterCursor;
}

boost::optional<ClientCursorPin> executeSingleExecUntilFirstBatch(
    const AggExState& aggExState,
    boost::intrusive_ptr<ExpressionContext> expCtx,
    std::vector<unique_ptr<PlanExecutor, PlanExecutor::Deleter>>& execs,
    rpc::ReplyBuilderInterface* result) {
    auto opCtx = aggExState.getOpCtx();
    boost::optional<ClientCursorPin> maybePinnedCursor;
    CursorResponseBuilder::Options options;
    options.isInitialResponse = true;
    if (!opCtx->inMultiDocumentTransaction()) {
        options.atClusterTime = repl::ReadConcernArgs::get(opCtx).getArgsAtClusterTime();
    }
    CursorResponseBuilder responseBuilder(result, options);

    auto cursorId = 0LL;
    const bool doRegisterCursor = getFirstBatch(aggExState, expCtx, *execs[0], responseBuilder);

    if (doRegisterCursor) {
        auto curOp = CurOp::get(opCtx);
        // Only register a cursor for the pipeline if we have found that we need one for future
        // calls to 'getMore()'. This cursor owns no collection state, and thus we register it with
        // the global cursor manager.
        maybePinnedCursor = registerCursor(aggExState, expCtx, std::move(execs[0]));
        auto cursor = maybePinnedCursor->getCursor();
        cursorId = cursor->cursorid();
        curOp->debug().cursorid = cursorId;

        // If a time limit was set on the pipeline, remaining time is "rolled over" to the
        // cursor (for use by future getmore ops).
        cursor->setLeftoverMaxTimeMicros(opCtx->getRemainingMaxTimeMicros());
    }

    collectQueryStats(aggExState, expCtx, execs[0].get(), maybePinnedCursor.get_ptr());

    boost::optional<CursorMetrics> metrics = aggExState.getRequest().getIncludeQueryStatsMetrics()
        ? boost::make_optional(CurOp::get(opCtx)->debug().getCursorMetrics())
        : boost::none;
    responseBuilder.done(
        cursorId,
        aggExState.getOriginalNss(),
        std::move(metrics),
        SerializationContext::stateCommandReply(aggExState.getRequest().getSerializationContext()));

    return maybePinnedCursor;
}

/**
 * Executes the aggregation pipeline, registering any cursors needed for subsequent calls to
 * getMore() if necessary.
 */
void executeUntilFirstBatch(const AggExState& aggExState,
                            AggCatalogState& aggCatalogState,
                            boost::intrusive_ptr<ExpressionContext> expCtx,
                            std::vector<unique_ptr<PlanExecutor, PlanExecutor::Deleter>>& execs,
                            rpc::ReplyBuilderInterface* result) {
    // If any cursor is registered, this will be the first ClientCursorPin.
    boost::optional<ClientCursorPin> maybePinnedCursor = boost::none;

    if (execs.size() == 1) {
        maybePinnedCursor = executeSingleExecUntilFirstBatch(aggExState, expCtx, execs, result);
    } else {
        // We disallowed external data sources in queries with multiple plan executors due to a data
        // race (see SERVER-85453 for more details).
        tassert(8545301,
                "External data sources are not currently compatible with queries that use multiple "
                "plan executors.",
                aggExState.getExternalDataSourceScopeGuard() == nullptr);

        // If there is more than one executor, that means this query will be running on multiple
        // shards via exchange and merge. Such queries always require a cursor to be registered for
        // each PlanExecutor.
        std::vector<ClientCursorPin> pinnedCursors;
        for (auto&& exec : execs) {
            auto pinnedCursor = registerCursor(aggExState, expCtx, std::move(exec));

            // The first executor is the main executor. The following ones are additionalExecutors.
            // AdditionalExecutors must never have associated ShardRole resources – therefore, we
            // stash empty TransactionResources to their stashed cursor.
            if (!pinnedCursors.empty() &&
                pinnedCursor->getExecutor()->lockPolicy() ==
                    PlanExecutor::LockPolicy::kLocksInternally) {
                pinnedCursor->stashTransactionResources(StashedTransactionResources{
                    std::make_unique<shard_role_details::TransactionResources>(),
                    shard_role_details::TransactionResources::State::EMPTY});
            }
            pinnedCursors.emplace_back(std::move(pinnedCursor));
        }
        handleMultipleCursorsForExchange(aggExState, expCtx, pinnedCursors, result);

        if (pinnedCursors.size() > 0) {
            maybePinnedCursor = std::move(pinnedCursors[0]);
        }
    }

    // For an optimized away pipeline, signal the cache that a query operation has completed.
    // For normal pipelines this is done in DocumentSourceCursor.
    if (aggCatalogState.lockAcquired()) {
        auto exec =
            maybePinnedCursor ? maybePinnedCursor->getCursor()->getExecutor() : execs[0].get();
        const auto& planExplainer = exec->getPlanExplainer();
        if (const auto& coll = aggCatalogState.getMainCollectionOrView().getCollectionPtr()) {
            auto& debugInfo = CurOp::get(aggExState.getOpCtx())->debug();
            CollectionIndexUsageTrackerDecoration::recordCollectionIndexUsage(
                coll.get(),
                debugInfo.collectionScans,
                debugInfo.collectionScansNonTailable,
                debugInfo.indexesUsed);
        }
        // For SBE pushed down pipelines, we may need to report stats saved for secondary
        // collections separately.
        for (const auto& [secondaryNss, coll] :
             aggCatalogState.getCollections().getSecondaryCollections()) {
            if (coll) {
                PlanSummaryStats secondaryStats;
                planExplainer.getSecondarySummaryStats(secondaryNss, &secondaryStats);
                CollectionIndexUsageTrackerDecoration::recordCollectionIndexUsage(
                    coll.get(),
                    secondaryStats.collectionScans,
                    secondaryStats.collectionScansNonTailable,
                    secondaryStats.indexesUsed);
            }
        }
    }

    // For optimized away pipelines, which use LockPolicy::kLockExternally, stash the ShardRole
    // TransactionResources on the cursor.
    if (aggCatalogState.lockAcquired() && maybePinnedCursor) {
        invariant(maybePinnedCursor->getCursor());
        aggCatalogState.stashResources(maybePinnedCursor->getCursor());
    }
}

/**
 * If the aggregation 'request' contains an exchange specification, create a new pipeline for each
 * consumer and put it into the resulting vector. Otherwise, return the original 'pipeline' as a
 * single vector element.
 */
std::vector<std::unique_ptr<Pipeline>> createExchangePipelinesIfNeeded(
    const AggExState& aggExState, std::unique_ptr<Pipeline> pipeline) {
    std::vector<std::unique_ptr<Pipeline>> pipelines;

    if (aggExState.getRequest().getExchange() && !pipeline->getContext()->getExplain()) {
        auto expCtx = pipeline->getContext();
        // The Exchange constructor deregisters the pipeline from the context. Since we need a valid
        // opCtx for the ExpressionContextBuilder call below, store the pointer ahead of the
        // Exchange() call.
        auto* opCtx = aggExState.getOpCtx();
        auto exchange = make_intrusive<exec::agg::Exchange>(
            aggExState.getRequest().getExchange().value(), std::move(pipeline));

        for (size_t idx = 0; idx < exchange->getConsumers(); ++idx) {
            // For every new pipeline we have create a new ExpressionContext as the context
            // cannot be shared between threads. There is no synchronization for pieces of
            // the execution machinery above the Exchange, so nothing above the Exchange can be
            // shared between different exchange-producer cursors.
            auto collator = expCtx->getCollator() ? expCtx->getCollator()->clone() : nullptr;
            expCtx = ExpressionContextBuilder{}
                         .fromRequest(aggExState.getOpCtx(),
                                      aggExState.getRequest(),
                                      allowDiskUseByDefault.load())
                         .collator(std::move(collator))
                         .collUUID(expCtx->getUUID())
                         .mongoProcessInterface(MongoProcessInterface::create(opCtx))
                         .mayDbProfile(CurOp::get(aggExState.getOpCtx())->dbProfileLevel() > 0)
                         .resolvedNamespace(uassertStatusOK(aggExState.resolveInvolvedNamespaces()))
                         .tmpDir(boost::filesystem::path(storageGlobalParams.dbpath) / "_tmp")
                         .collationMatchesDefault(expCtx->getCollationMatchesDefault())
                         .canBeRejected(query_settings::canPipelineBeRejected(
                             aggExState.getRequest().getPipeline()))
                         .build();
            // Create a new pipeline for the consumer consisting of a single
            // DocumentSourceExchange.
            const auto& resourceYielder = ResourceYielderFactory::get(*opCtx->getService());
            auto consumer = make_intrusive<DocumentSourceExchange>(
                expCtx,
                exchange,
                idx,
                // Assumes this is only called from the 'aggregate' or 'getMore' commands.  The code
                // which relies on this parameter does not distinguish/care about the difference so
                // we simply always pass 'aggregate'.
                resourceYielder ? resourceYielder->make(opCtx, "aggregate"_sd) : nullptr);
            pipelines.emplace_back(Pipeline::create({consumer}, expCtx));
        }
    } else {
        pipelines.emplace_back(std::move(pipeline));
    }

    return pipelines;
}

std::vector<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> prepareExecutors(
    const AggExState& aggExState,
    AggCatalogState& aggCatalogState,
    std::unique_ptr<Pipeline> pipeline) {
    const auto expCtx = pipeline->getContext();
    const auto mainCollectionUUID = aggCatalogState.getUUID();
    // Check if the pipeline has a $geoNear stage, as it will be ripped away during the build query
    // executor phase below (to be replaced with a $geoNearCursorStage later during the executor
    // attach phase).
    auto hasGeoNearStage =
        !pipeline->empty() && dynamic_cast<DocumentSourceGeoNear*>(pipeline->peekFront());

    // Prepare a PlanExecutor to provide input into the pipeline, if needed. Add additional
    // executors if needed to serve the aggregation (currently only includes search commands
    // that generate metadata).
    auto [executor, attachCallback, additionalExecutors] =
        PipelineD::buildInnerQueryExecutor(aggCatalogState.getCollections(),
                                           aggExState.getExecutionNss(),
                                           &aggExState.getRequest(),
                                           pipeline.get());

    std::vector<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> execs;
    if (canOptimizeAwayPipeline(aggExState, pipeline.get(), executor.get(), hasGeoNearStage)) {
        // This pipeline is currently empty, but once completed it will have only one source,
        // which is a DocumentSourceCursor. Instead of creating a whole pipeline to do nothing
        // more than forward the results of its cursor document source, we can use the
        // PlanExecutor by itself. The resulting cursor will look like what the client would
        // have gotten from find command.
        execs.emplace_back(std::move(executor));
    } else {
        // Complete creation of the initial $cursor stage, if needed.
        auto sharedStasher = make_intrusive<ShardRoleTransactionResourcesStasherForPipeline>();
        auto catalogResourceHandle = make_intrusive<DSCursorCatalogResourceHandle>(sharedStasher);
        PipelineD::attachInnerQueryExecutorToPipeline(aggCatalogState.getCollections(),
                                                      attachCallback,
                                                      std::move(executor),
                                                      pipeline.get(),
                                                      catalogResourceHandle);

        std::vector<std::unique_ptr<Pipeline>> pipelines;
        // Any pipeline that relies on calls to mongot requires additional setup.
        if (search_helpers::isMongotPipeline(pipeline.get())) {
            // Release locks early, before we generate the search pipeline, so that we don't hold
            // them during network calls to mongot. This is fine for search pipelines since they are
            // not reading any local (lock-protected) data in the main pipeline.
            // Stash the ShardRole TransactionResources on the 'sharedStasher' we shared with the
            // pipeline stages.
            aggCatalogState.stashResources(sharedStasher.get());

            pipelines.push_back(std::move(pipeline));

            // TODO SERVER-89546 extractDocsNeededBounds should be called internally within
            // DocumentSourceSearch optimization; that also means we'd be skipping that step when
            // optimization is off.
            auto bounds = extractDocsNeededBounds(*pipelines.back().get());
            auto metadataPipe = search_helpers::prepareSearchForTopLevelPipelineLegacyExecutor(
                expCtx,
                pipelines.back().get(),
                bounds,
                aggExState.getRequest().getCursor().getBatchSize());
            if (metadataPipe) {
                pipelines.push_back(std::move(metadataPipe));
            }
        } else {
            // Takes ownership of 'pipeline'.
            pipelines = createExchangePipelinesIfNeeded(aggExState, std::move(pipeline));
        }

        for (auto&& pipelineIt : pipelines) {
            // There are separate ExpressionContexts for each exchange pipeline, so make sure to
            // pass the pipeline's ExpressionContext to the plan executor factory.
            auto pipelineExpCtx = pipelineIt->getContext();
            execs.emplace_back(plan_executor_factory::make(
                std::move(pipelineExpCtx),
                std::move(pipelineIt),
                getResumableScanType(aggExState.getRequest(), aggExState.hasChangeStream())));
        }

        if (aggCatalogState.lockAcquired()) {
            // With the pipelines created, we can relinquish locks as they will manage the locks
            // internally further on. We still need to keep the lock for an optimized away pipeline
            // though, as we will be changing its lock policy to 'kLockExternally' (see details
            // below), and in order to execute the initial getNext() call in 'handleCursorCommand',
            // we need to hold the collection lock.
            // Stash the ShardRole TransactionResources on the 'sharedStasher' we shared with the
            // pipeline stages.
            aggCatalogState.stashResources(sharedStasher.get());
        }
    }

    for (auto& exec : additionalExecutors) {
        execs.emplace_back(std::move(exec));
    }

    tassert(6624353, "No executors", !execs.empty());

    {
        auto planSummary = execs[0]->getPlanExplainer().getPlanSummary();
        stdx::lock_guard<Client> lk(*aggExState.getOpCtx()->getClient());
        CurOp::get(aggExState.getOpCtx())->setPlanSummary(lk, std::move(planSummary));
        CurOp::get(aggExState.getOpCtx())->debug().queryFramework = execs[0]->getQueryFramework();
    }

    hangAfterCreatingAggregationPlan.executeIf(
        [](const auto&) { hangAfterCreatingAggregationPlan.pauseWhileSet(); },
        [&](const BSONObj& data) {
            return mainCollectionUUID && UUID::parse(data["uuid"]) == *mainCollectionUUID;
        });

    return execs;
}

void executeExplain(const AggExState& aggExState,
                    const AggCatalogState& aggCatalogState,
                    boost::intrusive_ptr<ExpressionContext> expCtx,
                    PlanExecutor* explainExecutor,
                    rpc::ReplyBuilderInterface* result) {
    // Capture diagnostics to be logged in the case of a failure.
    ScopedDebugInfo explainDiagnostics(
        "explainDiagnostics", diagnostic_printers::ExplainDiagnosticPrinter{explainExecutor});
    auto bodyBuilder = result->getBodyBuilder();
    if (auto pipelineExec = dynamic_cast<PlanExecutorPipeline*>(explainExecutor)) {
        Explain::explainPipeline(pipelineExec,
                                 true /* executePipeline */,
                                 *(expCtx->getExplain()),
                                 *aggExState.getDeferredCmd(),
                                 &bodyBuilder);
    } else {
        invariant(explainExecutor->getOpCtx() == aggExState.getOpCtx());
        // The explainStages() function for a non-pipeline executor may need to execute the plan
        // to collect statistics. If the PlanExecutor uses kLockExternally policy, the
        // appropriate collection lock must be already held. Make sure it has not been released
        // yet.
        invariant(aggCatalogState.lockAcquired());
        Explain::explainStages(explainExecutor,
                               aggCatalogState.getCollections(),
                               *(expCtx->getExplain()),
                               BSON("optimizedPipeline" << true),
                               SerializationContext::stateCommandReply(
                                   aggExState.getRequest().getSerializationContext()),
                               *aggExState.getDeferredCmd(),
                               &bodyBuilder);
    }
    collectQueryStatsMongod(
        aggExState.getOpCtx(),
        expCtx,
        std::move(CurOp::get(aggExState.getOpCtx())->debug().queryStatsInfo.key));
}

/**
 * Executes the aggregation 'request' given a properly constructed AggregationExecutionState,
 * which holds the request and all necessary supporting context to execute request.
 *
 * If the query over a view that's already been resolved, the appropriate state should
 * have already been set in AggregationExecutionState.
 *
 * On success, fills out 'result' with the command response.
 */
Status _runAggregate(AggExState& aggExState, rpc::ReplyBuilderInterface* result);

/**
 * Resolve the view by finding the underlying collection and stitching the view pipelines and this
 * request's pipeline together. We then release our locks before recursively calling runAggregate(),
 * which will re-acquire locks on the underlying collection. (The lock must be released because
 * recursively acquiring locks on the database will prohibit yielding.)
 */
Status runAggregateOnView(ResolvedViewAggExState& resolvedViewAggExState,
                          std::unique_ptr<AggCatalogState> aggCatalogState,
                          rpc::ReplyBuilderInterface* result) {
    uassert(ErrorCodes::CommandNotSupportedOnView,
            "mapReduce on a view is not supported",
            !resolvedViewAggExState.getRequest().getIsMapReduceCommand());

    // Resolved view will be available after view has been set on AggregationExecutionState
    auto resolvedView = resolvedViewAggExState.getResolvedView();

    // With the view & collation resolved, we can relinquish locks.
    aggCatalogState->relinquishResources();

    OperationContext* opCtx = resolvedViewAggExState.getOpCtx();
    auto& originalNss = resolvedViewAggExState.getOriginalNss();

    auto status{Status::OK()};
    if (!OperationShardingState::get(opCtx).shouldBeTreatedAsFromRouter(opCtx)) {
        // Non sharding-aware operation.
        // Run the translated query on the view on this node.
        status = _runAggregate(resolvedViewAggExState, result);
    } else {
        // Sharding-aware operation.
        const auto& resolvedViewNss = resolvedView.getNamespace();

        // Stash the shard role for the resolved view nss, in case it was set, as we are about to
        // transition into the router role for it.
        const ScopedStashShardRole scopedUnsetShardRole{opCtx, resolvedViewNss};

        sharding::router::CollectionRouter router(opCtx->getServiceContext(),
                                                  resolvedViewNss,
                                                  false  // retryOnStaleShard=false
        );
        status = router.routeWithRoutingContext(
            opCtx, "runAggregateOnView", [&](OperationContext* opCtx, RoutingContext& routingCtx) {
                // TODO: SERVER-77402 Use a ShardRoleLoop here and remove this usage of
                // CollectionRouter's retryOnStaleShard=false.
                const auto& cri = routingCtx.getCollectionRoutingInfo(resolvedViewNss);

                // Setup the opCtx's OperationShardingState with the expected placement versions for
                // the underlying collection. Use the same 'placementConflictTime' from the original
                // request, if present.
                const auto scopedShardRole = resolvedViewAggExState.setShardRole(cri);

                // Mark routing table as validated as we have entered the shard role for a local
                // read.
                routingCtx.onRequestSentForNss(resolvedViewNss);

                // If the underlying collection is unsharded and is located on this shard, then we
                // can execute the view aggregation locally. Otherwise, we need to kick-back to the
                // router.
                if (!resolvedViewAggExState.canReadUnderlyingCollectionLocally(cri)) {
                    // Cannot execute the resolved aggregation locally. The router must do it.
                    //
                    // Before throwing the kick-back exception, validate the routing table
                    // we are basing this decision on. We do so by briefly entering into
                    // the shard-role by acquiring the underlying collection.
                    const auto underlyingColl = acquireCollectionMaybeLockFree(
                        opCtx,
                        CollectionAcquisitionRequest::fromOpCtx(
                            opCtx,
                            resolvedView.getNamespace(),
                            AcquisitionPrerequisites::OperationType::kRead));

                    // Throw the kick-back exception.
                    uasserted(std::move(resolvedView),
                              "Resolved views on collections that do not exclusively live on the "
                              "db-primary shard must be executed by mongos");
                }

                // Run the resolved aggregation locally.
                return _runAggregate(resolvedViewAggExState, result);
            });
    }

    // Set the namespace of the curop back to the view namespace so ctx records stats on this view
    // namespace on destruction.
    {
        // It's possible this resolvedViewAggExState will be unusable by the time _runAggregate
        // returns, so we must use opCtx and originalNss variables instead of trying to retrieve
        // from resolvedViewAggExState.
        // TODO SERVER-93536 Clarify ownership of aggExState.
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->setNS(lk, originalNss);
    }

    return status;
}

std::unique_ptr<Pipeline> parsePipelineAndRegisterQueryStats(
    const AggExState& aggExState,
    const AggCatalogState& aggCatalogState,
    boost::intrusive_ptr<ExpressionContext> expCtx) {
    // If applicable, ensure that the resolved namespace is added to the resolvedNamespaces map on
    // the expCtx before calling Pipeline::parse(). This is necessary for search on views as
    // Pipeline::parse() will first check if a view exists directly on the stage specification and
    // if none is found, will then check for the view using the expCtx. As such, it's necessary to
    // add the resolved namespace to the expCtx prior to any call to Pipeline::parse().
    auto* opCtx = expCtx->getOperationContext();
    const bool isHybridSearchPipeline = aggExState.isHybridSearchPipeline();
    if (isHybridSearchPipeline) {
        uassert(10557301,
                "$rankFusion and $scoreFusion are unsupported on timeseries collections",
                !aggCatalogState.isTimeseries());
    }
    if (aggExState.isView()) {
        search_helpers::checkAndSetViewOnExpCtx(expCtx,
                                                aggExState.getOriginalRequest().getPipeline(),
                                                aggExState.getResolvedView(),
                                                aggExState.getOriginalNss());

        if (isHybridSearchPipeline) {
            uassert(ErrorCodes::OptionNotSupportedOnView,
                    "$rankFusion and $scoreFusion are currently unsupported on views",
                    feature_flags::gFeatureFlagSearchHybridScoringFull
                        .isEnabledUseLatestFCVWhenUninitialized(
                            VersionContext::getDecoration(opCtx),
                            serverGlobalParams.featureCompatibility.acquireFCVSnapshot()));

            // This insertion into the ExpressionContext ResolvedNamespaceMap is to handle cases
            // where the original query desugars into a $unionWith that runs on a view (like
            // $rankFusion and $scoreFusion). After view resolution (here), we treat the query as if
            // it was running on the underlying collection, with the view pipeline already
            // pre-pended to the top of original query, so we don't insert this mapping into all
            // queries by default. Further, if the original query had a $unionWith on a view, that
            // ResolvedNamespaceMap entry would already be added at LiteParseing. So in the specific
            // cases where we have a stage that desugars into $unionWith on a view, this insertion
            // is necessary.
            //
            // Also, in the Hybrid Search case, we know that view that the $unionWith will run on
            // will be the same one the entire query is running on (whereas you could conceive of a
            // situation where a stage desugars into a $unionWith that runs on a different view as
            // the top-level query view), so we gate this call to only happen during Hybrid Search
            // queries.
            expCtx->addResolvedNamespace(
                aggExState.getOriginalNss(),
                ResolvedNamespace(aggExState.getResolvedView().getNamespace(),
                                  aggExState.getResolvedView().getPipeline(),
                                  aggCatalogState.getUUID(),
                                  true /*involvedNamespaceIsAView*/));
        }
    }

    // The query shape captured in query stats should reflect the original user request as close as
    // possible. If we're operating over a view, we first parse just the original user-given request
    // for the sake of registering query stats. Then, we'll parse the view pipeline and stitch
    // the two pipelines together below.
    auto userRequest = aggExState.getOriginalRequest();
    expCtx->startExpressionCounters();
    auto pipeline = Pipeline::parse(userRequest.getPipeline(), expCtx);
    expCtx->stopExpressionCounters();

    // Compute QueryShapeHash and record it in CurOp.
    query_shape::DeferredQueryShape deferredShape{[&]() {
        return shape_helpers::tryMakeShape<query_shape::AggCmdShape>(
            userRequest,
            aggExState.getOriginalNss(),
            aggExState.getInvolvedNamespaces(),
            *pipeline,
            expCtx);
    }};
    auto queryShapeHash =
        shape_helpers::computeQueryShapeHash(expCtx, deferredShape, aggExState.getOriginalNss());
    CurOp::get(opCtx)->debug().setQueryShapeHashIfNotPresent(opCtx, queryShapeHash);

    // Perform the query settings lookup and attach it to 'expCtx'.
    auto& querySettingsService = query_settings::QuerySettingsService::get(opCtx);
    auto querySettings = querySettingsService.lookupQuerySettingsWithRejectionCheck(
        expCtx, queryShapeHash, aggExState.getOriginalNss(), userRequest.getQuerySettings());
    expCtx->setQuerySettingsIfNotPresent(std::move(querySettings));

    // Register query stats with the pre-optimized pipeline. Exclude queries with encrypted fields
    // as indicated by the inclusion of encryptionInformation in the request. We still collect query
    // stats on collection-less aggregations.
    if (!aggExState.getRequest().getEncryptionInformation()) {
        // If this is a query over a resolved view, we want to register query stats with the
        // original user-given request and pipeline, rather than the new request generated when
        // resolving the view.
        auto collectionType = aggCatalogState.determineCollectionType();
        NamespaceStringSet pipelineInvolvedNamespaces(aggExState.getInvolvedNamespaces());
        query_stats::registerRequest(
            aggExState.getOpCtx(),
            aggExState.getOriginalNss(),
            [&]() {
                uassertStatusOKWithContext(deferredShape->getStatus(),
                                           "Failed to compute query shape");
                return std::make_unique<query_stats::AggKey>(expCtx,
                                                             userRequest,
                                                             std::move(deferredShape->getValue()),
                                                             std::move(pipelineInvolvedNamespaces),
                                                             collectionType);
            },
            aggExState.hasChangeStream());

        if (aggExState.getRequest().getIncludeQueryStatsMetrics()) {
            CurOp::get(aggExState.getOpCtx())->debug().queryStatsInfo.metricsRequested = true;
        }
    }

    if (aggExState.isView()) {
        expCtx->startExpressionCounters();
        // Knowing that the aggregation is a view, overwrite the pipeline.
        pipeline =
            aggExState.applyViewToPipeline(expCtx, std::move(pipeline), aggCatalogState.getUUID());
        expCtx->stopExpressionCounters();
    }

    // Validate the entire pipeline with the view definition.
    if (aggCatalogState.lockAcquired()) {
        pipeline->validateWithCollectionMetadata(aggCatalogState.getMainCollectionOrView());
    }

    expCtx->initializeReferencedSystemVariables();

    // Report usage statistics for each stage in the pipeline.
    aggExState.tickGlobalStageCounters();

    return pipeline;
}

StatusWith<std::unique_ptr<Pipeline>> preparePipeline(
    const AggExState& aggExState,
    const AggCatalogState& aggCatalogState,
    const boost::intrusive_ptr<ExpressionContext>& expCtx) {
    std::unique_ptr<Pipeline> pipeline =
        parsePipelineAndRegisterQueryStats(aggExState, aggCatalogState, expCtx);

    // Start the query planning timer right after parsing.
    CurOp::get(aggExState.getOpCtx())->beginQueryPlanningTimer();
    CurOp::get(aggExState.getOpCtx())->debug().isChangeStreamQuery = aggExState.hasChangeStream();

    if (expCtx->getServerSideJsConfig().accumulator && _samplerAccumulatorJs.tick()) {
        LOGV2_WARNING(
            8996502,
            "$accumulator is deprecated. For more information, see "
            "https://www.mongodb.com/docs/manual/reference/operator/aggregation/accumulator/");
    }

    if (expCtx->getServerSideJsConfig().function && _samplerFunctionJs.tick()) {
        LOGV2_WARNING(
            8996503,
            "$function is deprecated. For more information, see "
            "https://www.mongodb.com/docs/manual/reference/operator/aggregation/function/");
    }

    // Only allow the use of runtime constants when 'fromRouter' is true.
    uassert(463840,
            "Manually setting 'runtimeConstants' is not supported. Use 'let' for user-defined "
            "constants.",
            expCtx->getFromRouter() || !aggExState.getRequest().getLegacyRuntimeConstants());

    if (!aggExState.getRequest().getAllowDiskUse().value_or(true)) {
        allowDiskUseFalseCounter.increment();
    }

    auto pipelineCollationStatus = aggExState.collatorCompatibleWithPipeline(expCtx->getCollator());
    if (!pipelineCollationStatus.isOK()) {
        return pipelineCollationStatus;
    }

    // After parsing the pipeline and registering query stats, we must perform pre optimization
    // rewrites. The only rewrites supported are for viewless timeseries collections.
    if (aggCatalogState.lockAcquired()) {
        aggregation_hint_translation::translateIndexHintIfRequired(
            expCtx, aggCatalogState.getMainCollectionOrView(), aggExState.getRequest());
        pipeline->performPreOptimizationRewrites(expCtx, aggCatalogState.getMainCollectionOrView());
    }

    // If the aggregate command supports encrypted collections, do rewrites of the pipeline to
    // support querying against encrypted fields.
    if (prepareForFLERewrite(aggExState.getOpCtx(),
                             aggExState.getRequest().getEncryptionInformation())) {
        pipeline = processFLEPipelineD(aggExState.getOpCtx(),
                                       aggExState.getExecutionNss(),
                                       aggExState.getRequest().getEncryptionInformation().value(),
                                       std::move(pipeline));
        aggExState.getRequest().getEncryptionInformation()->setCrudProcessed(true);
    }

    if (search_helpers::isMongotPipeline(pipeline.get())) {
        // Before preparing the pipeline executor, we need to do dependency analysis to validate
        // the metadata dependencies.
        // TODO SERVER-40900 Consider performing $meta validation for all queries at this point
        // before optimization.
        pipeline->validateMetaDependencies();

        uassert(6253506,
                "Cannot have exchange specified in a search pipeline",
                !aggExState.getRequest().getExchange());
    }

    pipeline->optimizePipeline();

    constexpr bool alreadyOptimized = true;
    pipeline->validateCommon(alreadyOptimized);

    if (auto sampleId = analyze_shard_key::getOrGenerateSampleId(
            aggExState.getOpCtx(),
            expCtx->getNamespaceString(),
            analyze_shard_key::SampledCommandNameEnum::kAggregate,
            aggExState.getRequest())) {
        analyze_shard_key::QueryAnalysisWriter::get(aggExState.getOpCtx())
            ->addAggregateQuery(*sampleId,
                                expCtx->getNamespaceString(),
                                pipeline->getInitialQuery(),
                                expCtx->getCollatorBSON(),
                                aggExState.getRequest().getLet())
            .getAsync([](auto) {});
    }

    return std::move(pipeline);
}

Status _runAggregate(AggExState& aggExState, rpc::ReplyBuilderInterface* result) {
    // Perform the validation checks on the request and its derivatives before proceeding.
    aggExState.performValidationChecks();

    // If we are running a retryable write without shard key, check if the write was applied on this
    // shard, and if so, return early with an empty cursor with $_wasStatementExecuted
    // set to true.
    if (checkRetryableWriteAlreadyApplied(aggExState, result)) {
        return Status::OK();
    }

    // Going forward this operation must never ignore interrupt signals while waiting for lock
    // acquisition. This InterruptibleLockGuard will ensure that waiting for lock re-acquisition
    // after yielding will not ignore interrupt signals. This is necessary to avoid deadlocking with
    // replication rollback, which at the storage layer waits for all cursors to be closed under the
    // global MODE_X lock, after having sent interrupt signals to read operations. This operation
    // must never hold open storage cursors while ignoring interrupt.
    InterruptibleLockGuard interruptibleLockAcquisition(aggExState.getOpCtx());

    // Acquire any catalog locks needed by the pipeline, and create catalog-dependent state.
    std::unique_ptr<AggCatalogState> aggCatalogState = aggExState.createAggCatalogState();

    BSONObj shardKey = BSONObj();
    if (aggCatalogState->lockAcquired() &&
        aggCatalogState->getMainCollectionOrView().isCollection()) {
        const auto& mainCollShardingDescription =
            aggCatalogState->getMainCollectionOrView().getCollection().getShardingDescription();
        if (mainCollShardingDescription.isSharded()) {
            shardKey = mainCollShardingDescription.getShardKeyPattern().toBSON();
        }
    }
    // Create an RAII object that prints the collection's shard key in the case of a tassert
    // or crash.
    ScopedDebugInfo shardKeyDiagnostics("ShardKeyDiagnostics",
                                        diagnostic_printers::ShardKeyDiagnosticPrinter{shardKey});

    boost::optional<AutoStatsTracker> statsTracker;
    aggCatalogState->getStatsTrackerIfNeeded(statsTracker);

    // If this is a view, we must resolve the view, then recursively call runAggregate from
    // runAggregateOnView.
    if (aggCatalogState->lockAcquired() && aggCatalogState->getMainCollectionOrView().isView()) {
        // We do not need to expand the view pipeline when there is a $collStats stage, as
        // $collStats is supported on a view namespace. For a time-series collection, however,
        // the view is abstracted out for the users, so we needed to resolve the namespace to
        // get the underlying bucket collection.
        const auto& view = aggCatalogState->getMainCollectionOrView().getView();

        bool shouldViewBeExpanded =
            !aggExState.startsWithCollStats() || view.getViewDefinition().timeseries();
        if (shouldViewBeExpanded) {
            // "Convert" aggExState into resolvedViewAggExState. Note that this will make the
            // initial aggExState object unusable.
            auto resolvedViewAggExState =
                ResolvedViewAggExState::create(std::move(aggExState), aggCatalogState);
            if (!resolvedViewAggExState.isOK()) {
                return resolvedViewAggExState.getStatus();
            }

            return runAggregateOnView(
                *resolvedViewAggExState.getValue(), std::move(aggCatalogState), result);
        }
    }

    boost::intrusive_ptr<ExpressionContext> expCtx = aggCatalogState->createExpressionContext();

    // Create an RAII object that prints useful information about the ExpressionContext in the
    // case of a tassert or crash.
    ScopedDebugInfo expCtxDiagnostics("ExpCtxDiagnostics",
                                      diagnostic_printers::ExpressionContextPrinter{expCtx});

    // Prepare the parsed pipeline for execution. This involves parsing the pipeline,
    // registering query stats, rewriting the pipeline to support queryable encryption, and
    // optimizing and rewriting the pipeline if necessary.
    StatusWith<std::unique_ptr<Pipeline>> swPipeline =
        preparePipeline(aggExState, *aggCatalogState, expCtx);
    if (!swPipeline.isOK()) {
        return swPipeline.getStatus();
    }

    std::vector<unique_ptr<PlanExecutor, PlanExecutor::Deleter>> execs =
        prepareExecutors(aggExState, *aggCatalogState, std::move(swPipeline.getValue()));

    // Dispose of the statsTracker to update stats for Top and CurOp.
    statsTracker.reset();

    // Having released the collection lock, we can now begin to fetch results from the pipeline.
    // If both explain and cursor are specified, explain wins.
    if (expCtx->getExplain()) {
        executeExplain(aggExState, *aggCatalogState, expCtx, execs[0].get(), result);
    } else {
        executeUntilFirstBatch(aggExState, *aggCatalogState, expCtx, execs, result);
    }

    return Status::OK();
}

}  // namespace

// TODO SERVER-93536 take these variables in by rvalue to take internal ownership of them.
Status runAggregate(
    OperationContext* opCtx,
    AggregateCommandRequest& request,
    const LiteParsedPipeline& liteParsedPipeline,
    const BSONObj& cmdObj,
    const PrivilegeVector& privileges,
    boost::optional<ExplainOptions::Verbosity> verbosity,
    rpc::ReplyBuilderInterface* result,
    const std::vector<std::pair<NamespaceString, std::vector<ExternalDataSourceInfo>>>&
        usedExternalDataSources) {
    AggExState aggExState(
        opCtx, request, liteParsedPipeline, cmdObj, privileges, usedExternalDataSources, verbosity);

    // NOTE: It's possible this aggExState will be unusable by the time _runAggregate returns.
    // TODO SERVER-93536 Clarify ownership of aggExState.
    Status status = _runAggregate(aggExState, result);

    // The aggregation pipeline may change the namespace of the curop and we need to set it back to
    // the original namespace to correctly report command stats. One example when the namespace can
    // be changed is when the pipeline contains an $out stage, which executes an internal command to
    // create a temp collection, changing the curop namespace to the name of this temp collection.
    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->setNS(lk, request.getNamespace());
    }
    return status;
}
}  // namespace mongo
