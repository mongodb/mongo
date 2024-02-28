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

#include "mongo/db/commands/run_aggregate.h"

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
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <iosfwd>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "mongo/base/error_codes.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/simple_bsonobj_comparator.h"
#include "mongo/client/read_preference.h"
#include "mongo/crypto/fle_field_schema_gen.h"
#include "mongo/db/api_parameters.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/basic_types.h"
#include "mongo/db/basic_types_gen.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/collection_catalog.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/catalog/collection_uuid_mismatch.h"
#include "mongo/db/catalog/external_data_source_scope_guard.h"
#include "mongo/db/catalog_raii.h"
#include "mongo/db/change_stream_serverless_helpers.h"
#include "mongo/db/client.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/cluster_role.h"
#include "mongo/db/curop.h"
#include "mongo/db/cursor_id.h"
#include "mongo/db/cursor_manager.h"
#include "mongo/db/database_name.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/disk_use_options_gen.h"
#include "mongo/db/fle_crud.h"
#include "mongo/db/logical_time.h"
#include "mongo/db/namespace_string.h"
#include "mongo/db/pipeline/aggregation_request_helper.h"
#include "mongo/db/pipeline/change_stream_invalidation_info.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_exchange.h"
#include "mongo/db/pipeline/document_source_geo_near.h"
#include "mongo/db/pipeline/document_source_project.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/initialize_auto_get_helper.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/pipeline_d.h"
#include "mongo/db/pipeline/plan_executor_pipeline.h"
#include "mongo/db/pipeline/process_interface/mongo_process_interface.h"
#include "mongo/db/pipeline/search/search_helper.h"
#include "mongo/db/query/canonical_query.h"
#include "mongo/db/query/collation/collation_spec.h"
#include "mongo/db/query/collation/collator_interface.h"
#include "mongo/db/query/collection_query_info.h"
#include "mongo/db/query/cqf_command_utils.h"
#include "mongo/db/query/cqf_fast_paths.h"
#include "mongo/db/query/cqf_get_executor.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/explain.h"
#include "mongo/db/query/explain_options.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/query/multiple_collection_accessor.h"
#include "mongo/db/query/optimizer/defs.h"
#include "mongo/db/query/plan_executor.h"
#include "mongo/db/query/plan_executor_factory.h"
#include "mongo/db/query/plan_explainer.h"
#include "mongo/db/query/plan_summary_stats.h"
#include "mongo/db/query/query_decorations.h"
#include "mongo/db/query/query_knobs_gen.h"
#include "mongo/db/query/query_request_helper.h"
#include "mongo/db/query/query_settings/query_settings_utils.h"
#include "mongo/db/query/query_shape/agg_cmd_shape.h"
#include "mongo/db/query/query_stats/agg_key.h"
#include "mongo/db/query/query_stats/key.h"
#include "mongo/db/query/query_stats/query_stats.h"
#include "mongo/db/query/util/deferred.h"
#include "mongo/db/read_concern.h"
#include "mongo/db/read_write_concern_provenance.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/db/repl/read_concern_level.h"
#include "mongo/db/repl/tenant_migration_access_blocker_util.h"
#include "mongo/db/s/operation_sharding_state.h"
#include "mongo/db/s/query_analysis_writer.h"
#include "mongo/db/server_options.h"
#include "mongo/db/server_parameter.h"
#include "mongo/db/stats/resource_consumption_metrics.h"
#include "mongo/db/stats/top.h"
#include "mongo/db/storage/recovery_unit.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/db/tenant_id.h"
#include "mongo/db/transaction/transaction_participant.h"
#include "mongo/db/transaction_resources.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/db/views/view.h"
#include "mongo/db/views/view_catalog_helpers.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_attr.h"
#include "mongo/logv2/log_component.h"
#include "mongo/logv2/redaction.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/s/analyze_shard_key_common_gen.h"
#include "mongo/s/query_analysis_sampler_util.h"
#include "mongo/s/resource_yielders.h"
#include "mongo/s/router_role.h"
#include "mongo/s/shard_version.h"
#include "mongo/s/sharding_state.h"
#include "mongo/s/stale_exception.h"
#include "mongo/stdx/unordered_set.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/decorable.h"
#include "mongo/util/future.h"
#include "mongo/util/intrusive_counter.h"
#include "mongo/util/namespace_string_util.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/serialization_context.h"
#include "mongo/util/str.h"
#include "mongo/util/string_map.h"
#include "mongo/util/synchronized_value.h"
#include "mongo/util/time_support.h"
#include "mongo/util/uuid.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kQuery

namespace mongo {

using boost::intrusive_ptr;
using std::shared_ptr;
using std::string;
using std::stringstream;
using std::unique_ptr;
using NamespaceStringSet = stdx::unordered_set<NamespaceString>;

Counter64& allowDiskUseFalseCounter = *MetricBuilder<Counter64>{"query.allowDiskUseFalse"};

namespace {

MONGO_FAIL_POINT_DEFINE(hangAfterCreatingAggregationPlan);
MONGO_FAIL_POINT_DEFINE(hangAfterAcquiringCollectionCatalog);

/**
 * If a pipeline is empty (assuming that a $cursor stage hasn't been created yet), it could mean
 * that we were able to absorb all pipeline stages and pull them into a single PlanExecutor. So,
 * instead of creating a whole pipeline to do nothing more than forward the results of its cursor
 * document source, we can optimize away the entire pipeline and answer the request using the query
 * engine only. This function checks if such optimization is possible.
 */
bool canOptimizeAwayPipeline(const Pipeline* pipeline,
                             const PlanExecutor* exec,
                             const AggregateCommandRequest& request,
                             bool hasGeoNearStage,
                             bool hasChangeStreamStage) {
    return pipeline && exec && !hasGeoNearStage && !hasChangeStreamStage &&
        pipeline->getSources().empty() &&
        // For exchange we will create a number of pipelines consisting of a single
        // DocumentSourceExchange stage, so cannot not optimize it away.
        !request.getExchange();
}

/**
 * Creates and registers a cursor with the global cursor manager. Returns the pinned cursor.
 */
ClientCursorPin registerCursor(OperationContext* opCtx,
                               const boost::intrusive_ptr<ExpressionContext>& expCtx,
                               const NamespaceString& nss,
                               const BSONObj& cmdObj,
                               const PrivilegeVector& privileges,
                               unique_ptr<PlanExecutor, PlanExecutor::Deleter> exec,
                               std::shared_ptr<ExternalDataSourceScopeGuard> extDataSrcGuard) {
    ClientCursorParams cursorParams(
        std::move(exec),
        nss,
        AuthorizationSession::get(opCtx->getClient())->getAuthenticatedUserName(),
        APIParameters::get(opCtx),
        opCtx->getWriteConcern(),
        repl::ReadConcernArgs::get(opCtx),
        ReadPreferenceSetting::get(opCtx),
        cmdObj,
        privileges);
    cursorParams.setTailableMode(expCtx->tailableMode);

    auto pin = CursorManager::get(opCtx)->registerCursor(opCtx, std::move(cursorParams));

    pin->incNBatches();
    if (extDataSrcGuard) {
        ExternalDataSourceScopeGuard::get(pin.getCursor()) = extDataSrcGuard;
    }
    return pin;
}

/**
 * Updates query stats in OpDebug using the plan explainer from the pinned cursor (if given)
 * or the given executor (otherwise) and collects them in the query stats store.
 */
void collectQueryStats(OperationContext* opCtx,
                       mongo::PlanExecutor* maybeExec,
                       ClientCursorPin* maybePinnedCursor) {
    invariant(maybeExec || maybePinnedCursor);

    auto curOp = CurOp::get(opCtx);
    const auto& planExplainer = maybePinnedCursor
        ? maybePinnedCursor->getCursor()->getExecutor()->getPlanExplainer()
        : maybeExec->getPlanExplainer();
    PlanSummaryStats stats;
    planExplainer.getSummaryStats(&stats);
    curOp->debug().setPlanSummaryMetrics(stats);
    curOp->setEndOfOpMetrics(stats.nReturned);

    if (maybePinnedCursor) {
        collectQueryStatsMongod(opCtx, *maybePinnedCursor);
    } else {
        collectQueryStatsMongod(opCtx, std::move(curOp->debug().queryStatsInfo.key));
    }
}

/**
 * Builds the reply for a pipeline over a sharded collection that contains an exchange stage.
 */
void handleMultipleCursorsForExchange(OperationContext* opCtx,
                                      const NamespaceString& nsForCursor,
                                      std::vector<ClientCursorPin>& pinnedCursors,
                                      const AggregateCommandRequest& request,
                                      rpc::ReplyBuilderInterface* result) {
    invariant(pinnedCursors.size() > 1);
    collectQueryStats(opCtx, nullptr, &pinnedCursors[0]);
    long long batchSize =
        request.getCursor().getBatchSize().value_or(aggregation_request_helper::kDefaultBatchSize);

    uassert(ErrorCodes::BadValue, "the exchange initial batch size must be zero", batchSize == 0);

    BSONArrayBuilder cursorsBuilder;
    for (auto&& pinnedCursor : pinnedCursors) {
        auto cursor = pinnedCursor.getCursor();
        invariant(cursor);

        BSONObjBuilder cursorResult;
        appendCursorResponseObject(
            cursor->cursorid(),
            nsForCursor,
            BSONArray(),
            cursor->getExecutor()->getExecutorType(),
            &cursorResult,
            SerializationContext::stateCommandReply(request.getSerializationContext()));
        cursorResult.appendBool("ok", 1);

        cursorsBuilder.append(cursorResult.obj());

        // If a time limit was set on the pipeline, remaining time is "rolled over" to the cursor
        // (for use by future getmore ops).
        cursor->setLeftoverMaxTimeMicros(opCtx->getRemainingMaxTimeMicros());

        // Cursor needs to be in a saved state while we yield locks for getmore. State will be
        // restored in getMore().
        cursor->getExecutor()->saveState();
        cursor->getExecutor()->detachFromOperationContext();
    }

    auto bodyBuilder = result->getBodyBuilder();
    bodyBuilder.appendArray("cursors", cursorsBuilder.obj());
}

/*
 * We use a Deferred<BSONObj> to accommodate aggregation on views. When aggregating on a
 * view, we create a new request using the resolved view and call back into _runAggregate(). The new
 * request (based on the resolved view) warrants a new command object, but this BSONObj is not
 * actually required unless we need to register a cursor (i.e if results do not fit in a single
 * batch). In some cases, serializing the command object can consume a significant amount of time
 * that we can optimize away if the results fit in a single batch.
 */
using DeferredCmd = DeferredFn<BSONObj>;

/**
 * Gets the first batch of documents produced by this pipeline by calling 'getNext()' on the
 * provided PlanExecutor. The provided CursorResponseBuilder will be populated with the batch.
 *
 * Returns true if we need to register a ClientCursor saved for this pipeline (for future getMore
 * requests). Otherwise, returns false.
 */
bool getFirstBatch(OperationContext* opCtx,
                   boost::intrusive_ptr<ExpressionContext> expCtx,
                   const AggregateCommandRequest& request,
                   const DeferredCmd& cmdObj,
                   PlanExecutor& exec,
                   CursorResponseBuilder& responseBuilder) {
    long long batchSize =
        request.getCursor().getBatchSize().value_or(aggregation_request_helper::kDefaultBatchSize);

    auto curOp = CurOp::get(opCtx);
    ResourceConsumption::DocumentUnitCounter docUnitsReturned;

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
                          "cmd"_attr = *cmdObj);

            exception.addContext("PlanExecutor error during aggregation");
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
        docUnitsReturned.observeOne(nextDoc.objsize());
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

    auto& metricsCollector = ResourceConsumption::MetricsCollector::get(opCtx);
    metricsCollector.incrementDocUnitsReturned(curOp->getNS(), docUnitsReturned);

    return doRegisterCursor;
}

boost::optional<ClientCursorPin> executeSingleExecUntilFirstBatch(
    OperationContext* opCtx,
    boost::intrusive_ptr<ExpressionContext> expCtx,
    const AggregateCommandRequest& request,
    const DeferredCmd& cmdObj,
    const PrivilegeVector& privileges,
    const NamespaceString& origNss,
    std::shared_ptr<ExternalDataSourceScopeGuard> extDataSrcGuard,
    std::vector<unique_ptr<PlanExecutor, PlanExecutor::Deleter>>& execs,
    rpc::ReplyBuilderInterface* result) {

    boost::optional<ClientCursorPin> maybePinnedCursor;

    CursorResponseBuilder::Options options;
    options.isInitialResponse = true;
    if (!opCtx->inMultiDocumentTransaction()) {
        options.atClusterTime = repl::ReadConcernArgs::get(opCtx).getArgsAtClusterTime();
    }
    CursorResponseBuilder responseBuilder(result, options);

    auto cursorId = 0LL;
    const bool doRegisterCursor =
        getFirstBatch(opCtx, expCtx, request, cmdObj, *execs[0], responseBuilder);

    if (doRegisterCursor) {
        auto curOp = CurOp::get(opCtx);
        // Only register a cursor for the pipeline if we have found that we need one for future
        // calls to 'getMore()'.
        maybePinnedCursor = registerCursor(
            opCtx, expCtx, origNss, *cmdObj, privileges, std::move(execs[0]), extDataSrcGuard);
        auto cursor = maybePinnedCursor->getCursor();
        cursorId = cursor->cursorid();
        curOp->debug().cursorid = cursorId;

        // If a time limit was set on the pipeline, remaining time is "rolled over" to the
        // cursor (for use by future getmore ops).
        cursor->setLeftoverMaxTimeMicros(opCtx->getRemainingMaxTimeMicros());
    }

    collectQueryStats(opCtx, execs[0].get(), maybePinnedCursor.get_ptr());

    boost::optional<CursorMetrics> metrics = request.getIncludeQueryStatsMetrics()
        ? boost::make_optional(CurOp::get(opCtx)->debug().getCursorMetrics())
        : boost::none;
    responseBuilder.done(
        cursorId,
        origNss,
        std::move(metrics),
        SerializationContext::stateCommandReply(request.getSerializationContext()));

    return maybePinnedCursor;
}

/**
 * Executes the aggregation pipeline, registering any cursors needed for subsequent calls to
 * getMore() if necessary. Returns the first ClientCursorPin, if any cursor was registered.
 */
boost::optional<ClientCursorPin> executeUntilFirstBatch(
    OperationContext* opCtx,
    boost::intrusive_ptr<ExpressionContext> expCtx,
    const AggregateCommandRequest& request,
    const DeferredCmd& cmdObj,
    const PrivilegeVector& privileges,
    const NamespaceString& origNss,
    std::shared_ptr<ExternalDataSourceScopeGuard> extDataSrcGuard,
    std::vector<unique_ptr<PlanExecutor, PlanExecutor::Deleter>>& execs,
    rpc::ReplyBuilderInterface* result) {

    if (execs.size() == 1) {
        return executeSingleExecUntilFirstBatch(
            opCtx, expCtx, request, cmdObj, privileges, origNss, extDataSrcGuard, execs, result);
    }

    // We disallowed external data sources in queries with multiple plan executors due to a data
    // race (see SERVER-85453 for more details).
    tassert(8545301,
            "External data sources are not currently compatible with queries that use multiple "
            "plan executors.",
            extDataSrcGuard == nullptr);

    // If there is more than one executor, that means this query will be running on multiple
    // shards via exchange and merge. Such queries always require a cursor to be registered for
    // each PlanExecutor.
    std::vector<ClientCursorPin> pinnedCursors;
    for (auto&& exec : execs) {
        auto pinnedCursor =
            registerCursor(opCtx, expCtx, origNss, *cmdObj, privileges, std::move(exec), nullptr);
        pinnedCursors.emplace_back(std::move(pinnedCursor));
    }
    handleMultipleCursorsForExchange(opCtx, origNss, pinnedCursors, request, result);

    if (pinnedCursors.size() > 0) {
        return std::move(pinnedCursors[0]);
    }

    return boost::none;
}

StatusWith<StringMap<ExpressionContext::ResolvedNamespace>> resolveInvolvedNamespaces(
    OperationContext* opCtx,
    const AggregateCommandRequest& request,
    const NamespaceStringSet& pipelineInvolvedNamespaces) {

    // If there are no involved namespaces, return before attempting to take any locks. This is
    // important for collectionless aggregations, which may be expected to run without locking.
    if (pipelineInvolvedNamespaces.empty()) {
        return {StringMap<ExpressionContext::ResolvedNamespace>()};
    }

    // Acquire a single const view of the CollectionCatalog and use it for all view and collection
    // lookups and view definition resolutions that follow. This prevents the view definitions
    // cached in 'resolvedNamespaces' from changing relative to those in the acquired ViewCatalog.
    // The resolution of the view definitions below might lead into an endless cycle if any are
    // allowed to change.
    auto catalog = CollectionCatalog::get(opCtx);

    std::deque<NamespaceString> involvedNamespacesQueue(pipelineInvolvedNamespaces.begin(),
                                                        pipelineInvolvedNamespaces.end());
    StringMap<ExpressionContext::ResolvedNamespace> resolvedNamespaces;

    while (!involvedNamespacesQueue.empty()) {
        auto involvedNs = std::move(involvedNamespacesQueue.front());
        involvedNamespacesQueue.pop_front();

        if (resolvedNamespaces.find(involvedNs.coll()) != resolvedNamespaces.end()) {
            continue;
        }

        // If 'ns' refers to a view namespace, then we resolve its definition.
        auto resolveViewDefinition = [&](const NamespaceString& ns) -> Status {
            auto resolvedView = view_catalog_helpers::resolveView(opCtx, catalog, ns, boost::none);
            if (!resolvedView.isOK()) {
                return resolvedView.getStatus().withContext(str::stream()
                                                            << "Failed to resolve view '"
                                                            << involvedNs.toStringForErrorMsg());
            }

            auto&& underlyingNs = resolvedView.getValue().getNamespace();
            // Attempt to acquire UUID of the underlying collection using lock free method.
            auto uuid = catalog->lookupUUIDByNSS(opCtx, underlyingNs);
            resolvedNamespaces[ns.coll()] = {
                underlyingNs, resolvedView.getValue().getPipeline(), uuid};

            // We parse the pipeline corresponding to the resolved view in case we must resolve
            // other view namespaces that are also involved.
            LiteParsedPipeline resolvedViewLitePipeline(resolvedView.getValue().getNamespace(),
                                                        resolvedView.getValue().getPipeline());

            const auto& resolvedViewInvolvedNamespaces =
                resolvedViewLitePipeline.getInvolvedNamespaces();
            involvedNamespacesQueue.insert(involvedNamespacesQueue.end(),
                                           resolvedViewInvolvedNamespaces.begin(),
                                           resolvedViewInvolvedNamespaces.end());
            return Status::OK();
        };

        // If the involved namespace is not in the same database as the aggregation, it must be
        // from a $lookup/$graphLookup into a tenant migration donor's oplog view or from an
        // $out/$merge to a collection in a different database.
        if (!involvedNs.isEqualDb(request.getNamespace())) {
            if (involvedNs == NamespaceString::kTenantMigrationOplogView) {
                // For tenant migrations, we perform an aggregation on 'config.transactions' but
                // require a lookup stage involving a view on the 'local' database.
                // If the involved namespace is 'local.system.tenantMigration.oplogView', resolve
                // its view definition.
                auto status = resolveViewDefinition(involvedNs);
                if (!status.isOK()) {
                    return status;
                }
            } else {
                // SERVER-51886: It is not correct to assume that we are reading from a collection
                // because the collection targeted by $out/$merge on a given database can have the
                // same name as a view on the source database. As such, we determine whether the
                // collection name references a view on the aggregation request's database. Note
                // that the inverse scenario (mistaking a view for a collection) is not an issue
                // because $merge/$out cannot target a view.
                auto nssToCheck = NamespaceStringUtil::deserialize(request.getNamespace().dbName(),
                                                                   involvedNs.coll());
                if (catalog->lookupView(opCtx, nssToCheck)) {
                    auto status = resolveViewDefinition(nssToCheck);
                    if (!status.isOK()) {
                        return status;
                    }
                } else {
                    resolvedNamespaces[involvedNs.coll()] = {involvedNs, std::vector<BSONObj>{}};
                }
            }
        } else if (catalog->lookupCollectionByNamespace(opCtx, involvedNs)) {
            // Attempt to acquire UUID of the collection using lock free method.
            auto uuid = catalog->lookupUUIDByNSS(opCtx, involvedNs);
            // If 'involvedNs' refers to a collection namespace, then we resolve it as an empty
            // pipeline in order to read directly from the underlying collection.
            resolvedNamespaces[involvedNs.coll()] = {involvedNs, std::vector<BSONObj>{}, uuid};
        } else if (catalog->lookupView(opCtx, involvedNs)) {
            auto status = resolveViewDefinition(involvedNs);
            if (!status.isOK()) {
                return status;
            }
        } else {
            // 'involvedNs' is neither a view nor a collection, so resolve it as an empty pipeline
            // to treat it as reading from a non-existent collection.
            resolvedNamespaces[involvedNs.coll()] = {involvedNs, std::vector<BSONObj>{}};
        }
    }

    return resolvedNamespaces;
}

/**
 * Returns Status::OK if each view namespace in 'pipeline' has a default collator equivalent to
 * 'collator'. Otherwise, returns ErrorCodes::OptionNotSupportedOnView.
 */
Status collatorCompatibleWithPipeline(OperationContext* opCtx,
                                      const CollatorInterface* collator,
                                      const LiteParsedPipeline& liteParsedPipeline) {
    auto catalog = CollectionCatalog::get(opCtx);
    for (const auto& potentialViewNs : liteParsedPipeline.getInvolvedNamespaces()) {
        if (catalog->lookupCollectionByNamespace(opCtx, potentialViewNs)) {
            continue;
        }

        auto view = catalog->lookupView(opCtx, potentialViewNs);
        if (!view) {
            continue;
        }
        if (!CollatorInterface::collatorsMatch(view->defaultCollator(), collator)) {
            return {ErrorCodes::OptionNotSupportedOnView,
                    str::stream() << "Cannot override a view's default collation"
                                  << potentialViewNs.toStringForErrorMsg()};
        }
    }
    return Status::OK();
}

boost::intrusive_ptr<ExpressionContext> makeExpressionContext(
    OperationContext* opCtx,
    const AggregateCommandRequest& request,
    std::unique_ptr<CollatorInterface> collator,
    boost::optional<UUID> uuid,
    ExpressionContext::CollationMatchesDefault collationMatchesDefault,
    const NamespaceStringSet& pipelineInvolvedNamespaces) {
    auto expCtx = make_intrusive<ExpressionContext>(
        opCtx,
        request,
        std::move(collator),
        MongoProcessInterface::create(opCtx),
        uassertStatusOK(resolveInvolvedNamespaces(opCtx, request, pipelineInvolvedNamespaces)),
        uuid,
        CurOp::get(opCtx)->dbProfileLevel() > 0,
        allowDiskUseByDefault.load());
    expCtx->tempDir = storageGlobalParams.dbpath + "/_tmp";
    expCtx->collationMatchesDefault = collationMatchesDefault;
    return expCtx;
}

/**
 * Upconverts the read concern for a change stream aggregation, if necesssary.
 *
 * If there is no given read concern level on the given object, upgrades the level to 'majority' and
 * waits for read concern. If a read concern level is already specified on the given read concern
 * object, this method does nothing.
 */
void _adjustChangeStreamReadConcern(OperationContext* opCtx) {
    repl::ReadConcernArgs& readConcernArgs = repl::ReadConcernArgs::get(opCtx);
    // There is already a non-default read concern level set. Do nothing.
    if (readConcernArgs.hasLevel() && !readConcernArgs.getProvenance().isImplicitDefault()) {
        return;
    }
    // We upconvert an empty read concern to 'majority'.
    {
        // We must obtain the client lock to set the ReadConcernArgs on the operation
        // context as it may be concurrently read by CurrentOp.
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        readConcernArgs = repl::ReadConcernArgs(repl::ReadConcernLevel::kMajorityReadConcern);

        // Change streams are allowed to use the speculative majority read mechanism, if
        // the storage engine doesn't support majority reads directly.
        if (!serverGlobalParams.enableMajorityReadConcern) {
            readConcernArgs.setMajorityReadMechanism(
                repl::ReadConcernArgs::MajorityReadMechanism::kSpeculative);
        }
    }

    // Wait for read concern again since we changed the original read concern.
    uassertStatusOK(waitForReadConcern(opCtx, readConcernArgs, DatabaseName(), true));
    setPrepareConflictBehaviorForReadConcern(
        opCtx, readConcernArgs, PrepareConflictBehavior::kIgnoreConflicts);
}

/**
 * If the aggregation 'request' contains an exchange specification, create a new pipeline for each
 * consumer and put it into the resulting vector. Otherwise, return the original 'pipeline' as a
 * single vector element.
 */
std::vector<std::unique_ptr<Pipeline, PipelineDeleter>> createExchangePipelinesIfNeeded(
    const AggregateCommandRequest& request,
    const NamespaceStringSet& pipelineInvolvedNamespaces,
    std::unique_ptr<Pipeline, PipelineDeleter> pipeline) {
    std::vector<std::unique_ptr<Pipeline, PipelineDeleter>> pipelines;

    if (request.getExchange() && !pipeline->getContext()->explain) {
        auto expCtx = pipeline->getContext();
        // The Exchange constructor deregisters the pipeline from the context. Since we need a valid
        // opCtx for the makeExpressionContext call below, store the pointer ahead of the Exchange()
        // call.
        auto* opCtx = expCtx->opCtx;
        auto exchange =
            make_intrusive<Exchange>(request.getExchange().value(), std::move(pipeline));

        for (size_t idx = 0; idx < exchange->getConsumers(); ++idx) {
            // For every new pipeline we have create a new ExpressionContext as the context
            // cannot be shared between threads. There is no synchronization for pieces of
            // the execution machinery above the Exchange, so nothing above the Exchange can be
            // shared between different exchange-producer cursors.
            expCtx = makeExpressionContext(opCtx,
                                           request,
                                           expCtx->getCollator() ? expCtx->getCollator()->clone()
                                                                 : nullptr,
                                           expCtx->uuid,
                                           expCtx->collationMatchesDefault,
                                           pipelineInvolvedNamespaces);

            // Create a new pipeline for the consumer consisting of a single
            // DocumentSourceExchange.
            auto consumer = make_intrusive<DocumentSourceExchange>(
                expCtx,
                exchange,
                idx,
                // Assumes this is only called from the 'aggregate' or 'getMore' commands.  The code
                // which relies on this parameter does not distinguish/care about the difference so
                // we simply always pass 'aggregate'.
                ResourceYielderFactory::get(*expCtx->opCtx->getService())
                    .make(expCtx->opCtx, "aggregate"_sd));
            pipelines.emplace_back(Pipeline::create({consumer}, expCtx));
        }
    } else {
        pipelines.emplace_back(std::move(pipeline));
    }

    return pipelines;
}

/**
 * Creates additional pipelines if needed to serve the aggregation. This includes additional
 * pipelines for exchange optimization and search commands that generate metadata. Returns
 * a vector of all pipelines needed for the query, including the original one.
 *
 * Takes ownership of the original, passed in, pipeline.
 */
std::vector<std::unique_ptr<Pipeline, PipelineDeleter>> createAdditionalPipelinesIfNeeded(
    const AggregateCommandRequest& request,
    const LiteParsedPipeline& liteParsedPipeline,
    std::unique_ptr<Pipeline, PipelineDeleter> pipeline,
    const std::function<void(void)>& resetContextFn) {

    std::vector<std::unique_ptr<Pipeline, PipelineDeleter>> pipelines;
    auto expCtx = pipeline->getContext();

    // Exchange is not allowed to be specified if there is a $search stage.
    if (search_helpers::isSearchPipeline(pipeline.get())) {
        // Release locks early, before we generate the search pipeline, so that we don't hold them
        // during network calls to mongot. This is fine for search pipelines since they are not
        // reading any local (lock-protected) data in the main pipeline.
        resetContextFn();
        pipelines.push_back(std::move(pipeline));

        if (auto metadataPipe = search_helpers::generateMetadataPipelineForSearch(
                expCtx->opCtx, expCtx, request, pipelines.back().get(), expCtx->uuid)) {
            pipelines.push_back(std::move(metadataPipe));
        }
    } else {
        // Takes ownership of 'pipeline'.
        pipelines = createExchangePipelinesIfNeeded(
            request, liteParsedPipeline.getInvolvedNamespaces(), std::move(pipeline));
    }
    return pipelines;
}

/**
 * Performs validations related to API versioning, time-series stages, and general command
 * validation.
 * Throws UserAssertion if any of the validations fails
 *     - validation of API versioning on each stage on the pipeline
 *     - validation of API versioning on 'AggregateCommandRequest' request
 *     - validation of time-series related stages
 *     - validation of command parameters
 */
void performValidationChecks(const OperationContext* opCtx,
                             const AggregateCommandRequest& request,
                             const LiteParsedPipeline& liteParsedPipeline) {
    liteParsedPipeline.validate(opCtx);
    aggregation_request_helper::validateRequestForAPIVersion(opCtx, request);
    aggregation_request_helper::validateRequestFromClusterQueryWithoutShardKey(request);
}

std::vector<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> createLegacyExecutor(
    std::unique_ptr<Pipeline, PipelineDeleter> pipeline,
    const LiteParsedPipeline& liteParsedPipeline,
    const NamespaceString& nss,
    const MultipleCollectionAccessor& collections,
    const AggregateCommandRequest& request,
    CurOp* curOp,
    const std::function<void(void)>& resetContextFn) {
    const auto expCtx = pipeline->getContext();
    // Check if the pipeline has a $geoNear stage, as it will be ripped away during the build query
    // executor phase below (to be replaced with a $geoNearCursorStage later during the executor
    // attach phase).
    auto hasGeoNearStage = !pipeline->getSources().empty() &&
        dynamic_cast<DocumentSourceGeoNear*>(pipeline->peekFront());

    // Prepare a PlanExecutor to provide input into the pipeline, if needed; and additional
    // executors if needed to serve the aggregation, this currently only includes search commands
    // that generate metadata.
    auto [executor, attachCallback, additionalExecutors] =
        PipelineD::buildInnerQueryExecutor(collections, nss, &request, pipeline.get());

    std::vector<std::unique_ptr<PlanExecutor, PlanExecutor::Deleter>> execs;
    if (canOptimizeAwayPipeline(pipeline.get(),
                                executor.get(),
                                request,
                                hasGeoNearStage,
                                liteParsedPipeline.hasChangeStream())) {
        // This pipeline is currently empty, but once completed it will have only one source,
        // which is a DocumentSourceCursor. Instead of creating a whole pipeline to do nothing
        // more than forward the results of its cursor document source, we can use the
        // PlanExecutor by itself. The resulting cursor will look like what the client would
        // have gotten from find command.
        execs.emplace_back(std::move(executor));
    } else {
        search_helpers::prepareSearchForTopLevelPipeline(pipeline.get());
        // Complete creation of the initial $cursor stage, if needed.
        PipelineD::attachInnerQueryExecutorToPipeline(
            collections, attachCallback, std::move(executor), pipeline.get());

        auto pipelines = createAdditionalPipelinesIfNeeded(
            request, liteParsedPipeline, std::move(pipeline), resetContextFn);
        for (auto&& pipelineIt : pipelines) {
            // There are separate ExpressionContexts for each exchange pipeline, so make sure to
            // pass the pipeline's ExpressionContext to the plan executor factory.
            auto pipelineExpCtx = pipelineIt->getContext();
            execs.emplace_back(
                plan_executor_factory::make(std::move(pipelineExpCtx),
                                            std::move(pipelineIt),
                                            aggregation_request_helper::getResumableScanType(
                                                request, liteParsedPipeline.hasChangeStream())));
        }

        // With the pipelines created, we can relinquish locks as they will manage the locks
        // internally further on. We still need to keep the lock for an optimized away pipeline
        // though, as we will be changing its lock policy to 'kLockExternally' (see details
        // below), and in order to execute the initial getNext() call in 'handleCursorCommand',
        // we need to hold the collection lock.
        resetContextFn();
    }

    for (auto& exec : additionalExecutors) {
        execs.emplace_back(std::move(exec));
    }
    return execs;
}

ScopedSetShardRole setShardRole(OperationContext* opCtx,
                                const NamespaceString& underlyingNss,
                                const NamespaceString& viewNss,
                                const CollectionRoutingInfo& cri) {
    const auto optPlacementConflictTimestamp = [&]() {
        auto originalShardVersion = OperationShardingState::get(opCtx).getShardVersion(viewNss);

        // Since for requests on timeseries namespaces the ServiceEntryPoint installs shard version
        // on the buckets collection instead of the viewNss.
        // TODO: SERVER-80719 Remove this.
        if (!originalShardVersion && underlyingNss.isTimeseriesBucketsCollection()) {
            originalShardVersion =
                OperationShardingState::get(opCtx).getShardVersion(underlyingNss);
        }

        return originalShardVersion ? originalShardVersion->placementConflictTime() : boost::none;
    }();


    if (cri.cm.hasRoutingTable()) {
        const auto myShardId = ShardingState::get(opCtx)->shardId();

        auto sv = cri.getShardVersion(myShardId);
        if (optPlacementConflictTimestamp) {
            sv.setPlacementConflictTime(*optPlacementConflictTimestamp);
        }
        return ScopedSetShardRole(
            opCtx, underlyingNss, sv /*shardVersion*/, boost::none /*databaseVersion*/);
    } else {
        auto sv = ShardVersion::UNSHARDED();
        if (optPlacementConflictTimestamp) {
            sv.setPlacementConflictTime(*optPlacementConflictTimestamp);
        }
        return ScopedSetShardRole(opCtx,
                                  underlyingNss,
                                  ShardVersion::UNSHARDED() /*shardVersion*/,
                                  cri.cm.dbVersion() /*databaseVersion*/);
    }
}

bool canReadUnderlyingCollectionLocally(OperationContext* opCtx, const CollectionRoutingInfo& cri) {
    const auto myShardId = ShardingState::get(opCtx)->shardId();
    const auto atClusterTime = repl::ReadConcernArgs::get(opCtx).getArgsAtClusterTime();

    const auto chunkManagerMaybeAtClusterTime =
        atClusterTime ? ChunkManager::makeAtTime(cri.cm, atClusterTime->asTimestamp()) : cri.cm;

    if (chunkManagerMaybeAtClusterTime.isSharded()) {
        return false;
    } else if (chunkManagerMaybeAtClusterTime.isUnsplittable()) {
        return chunkManagerMaybeAtClusterTime.getMinKeyShardIdWithSimpleCollation() == myShardId;
    } else {
        return chunkManagerMaybeAtClusterTime.dbPrimary() == myShardId;
    }
}

/**
 * Executes the aggregation 'request' over the specified namespace 'nss' using context 'opCtx'.
 *
 * The raw aggregate command parameters should be passed in 'cmdObj', and will be reported as the
 * originatingCommand in subsequent getMores on the resulting agg cursor.
 *
 * 'privileges' contains the privileges that were required to run this aggregation, to be used later
 * for re-checking privileges for getMore commands.
 *
 * If the query over a view that's already been resolved, the resolved view and the original
 * user-provided request both must be provided.
 *
 * On success, fills out 'result' with the command response.
 */
Status _runAggregate(OperationContext* opCtx,
                     AggregateCommandRequest& request,
                     const LiteParsedPipeline& liteParsedPipeline,
                     const DeferredCmd& cmdObj,
                     const PrivilegeVector& privileges,
                     rpc::ReplyBuilderInterface* result,
                     std::shared_ptr<ExternalDataSourceScopeGuard> externalDataSourceGuard,
                     boost::optional<const ResolvedView&> resolvedView,
                     boost::optional<const AggregateCommandRequest&> origRequest);

Status runAggregateOnView(OperationContext* opCtx,
                          const NamespaceString& origNss,
                          const AggregateCommandRequest& request,
                          const MultipleCollectionAccessor& collections,
                          boost::optional<std::unique_ptr<CollatorInterface>> collatorToUse,
                          const ViewDefinition* view,
                          std::shared_ptr<const CollectionCatalog> catalog,
                          const PrivilegeVector& privileges,
                          rpc::ReplyBuilderInterface* result,
                          const std::function<void(void)>& resetContextFn) {
    auto nss = request.getNamespace();

    uassert(ErrorCodes::CommandNotSupportedOnView,
            "mapReduce on a view is not supported",
            !request.getIsMapReduceCommand());

    // Check that the default collation of 'view' is compatible with the operation's
    // collation. The check is skipped if the request did not specify a collation.
    if (!request.getCollation().get_value_or(BSONObj()).isEmpty()) {
        invariant(collatorToUse);  // Should already be resolved at this point.
        if (!CollatorInterface::collatorsMatch(view->defaultCollator(), collatorToUse->get()) &&
            !view->timeseries()) {

            return {ErrorCodes::OptionNotSupportedOnView,
                    "Cannot override a view's default collation"};
        }
    }

    // Queries on timeseries views may specify non-default collation whereas queries
    // on all other types of views must match the default collator (the collation use
    // to originally create that collections). Thus in the case of operations on TS
    // views, we use the request's collation.
    auto timeSeriesCollator = view->timeseries() ? request.getCollation() : boost::none;

    auto resolvedView =
        uassertStatusOK(view_catalog_helpers::resolveView(opCtx, catalog, nss, timeSeriesCollator));

    // With the view & collation resolved, we can relinquish locks.
    resetContextFn();

    uassert(std::move(resolvedView),
            "Explain of a resolved view must be executed by mongos",
            !ShardingState::get(opCtx)->enabled() || !request.getExplain());

    // Parse the resolved view into a new aggregation request.
    auto newRequest = resolvedView.asExpandedViewAggregation(request);

    DeferredCmd deferredCmd{[&newRequest]() {
        return aggregation_request_helper::serializeToCommandObj(newRequest);
    }};

    auto status{Status::OK()};
    if (!OperationShardingState::get(opCtx).isComingFromRouter(opCtx)) {
        // Non sharding-aware operation.
        // Run the translated query on the view on this node.
        status = _runAggregate(opCtx,
                               newRequest,
                               {newRequest},
                               std::move(deferredCmd),
                               privileges,
                               result,
                               nullptr,
                               resolvedView,
                               request);
    } else {
        // Sharding-aware operation.

        // Stash the shard role for the resolved view nss, in case it was set, as we are about to
        // transition into the router role for it.
        const ScopedStashShardRole scopedUnsetShardRole{opCtx, resolvedView.getNamespace()};

        sharding::router::CollectionRouter router(opCtx->getServiceContext(),
                                                  resolvedView.getNamespace());
        status = router.route(
            opCtx,
            "runAggregateOnView",
            [&](OperationContext* opCtx, const CollectionRoutingInfo& cri) {
                const auto readUnderlyingCollectionLocally =
                    canReadUnderlyingCollectionLocally(opCtx, cri);

                // Setup the opCtx's OperationShardingState with the expected placement versions for
                // the underlying collection. Use the same 'placementConflictTime' from the original
                // request, if present.
                const auto scopedShardRole =
                    setShardRole(opCtx, resolvedView.getNamespace(), origNss, cri);

                // If the underlying collection is unsharded and is located on this shard, then we
                // can execute the view aggregation locally. Otherwise, we need to kick-back to the
                // router.
                if (readUnderlyingCollectionLocally) {
                    // Run the resolved aggregation locally.
                    return _runAggregate(opCtx,
                                         newRequest,
                                         {newRequest},
                                         std::move(deferredCmd),
                                         privileges,
                                         result,
                                         nullptr,
                                         resolvedView,
                                         request);
                } else {
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
            });
    }

    {
        // Set the namespace of the curop back to the view namespace so ctx records
        // stats on this view namespace on destruction.
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        CurOp::get(opCtx)->setNS_inlock(nss);
    }

    return status;
}

/**
 * Determines the collection type of the query by precedence of various configurations. The order
 * of these checks is critical since there may be overlap (e.g., a view over a virtual collection
 * is classified as a view).
 */
query_shape::CollectionType determineCollectionType(
    const boost::optional<AutoGetCollectionForReadCommandMaybeLockFree>& ctx,
    boost::optional<const ResolvedView&> resolvedView,
    bool hasChangeStream,
    bool isCollectionless) {
    if (resolvedView.has_value()) {
        if (resolvedView->timeseries()) {
            return query_shape::CollectionType::kTimeseries;
        }
        return query_shape::CollectionType::kView;
    }
    if (isCollectionless) {
        return query_shape::CollectionType::kVirtual;
    }
    if (hasChangeStream) {
        return query_shape::CollectionType::kChangeStream;
    }
    return ctx ? ctx->getCollectionType() : query_shape::CollectionType::kUnknown;
}

std::unique_ptr<Pipeline, PipelineDeleter> parsePipelineAndRegisterQueryStats(
    OperationContext* opCtx,
    const NamespaceString& origNss,
    const AggregateCommandRequest& request,
    const boost::optional<AutoGetCollectionForReadCommandMaybeLockFree>& ctx,
    std::unique_ptr<CollatorInterface> collator,
    boost::optional<UUID> uuid,
    ExpressionContext::CollationMatchesDefault collationMatchesDefault,
    const MultipleCollectionAccessor& collections,
    const LiteParsedPipeline& liteParsedPipeline,
    bool isCollectionless,
    boost::optional<const ResolvedView&> resolvedView,
    boost::optional<const AggregateCommandRequest&> origRequest) {
    // If we're operating over a view, we first parse just the original user-given request
    // for the sake of registering query stats. Then, we'll parse the view pipeline and stitch
    // the two pipelines together below.
    auto expCtx = makeExpressionContext(opCtx,
                                        request,
                                        std::move(collator),
                                        uuid,
                                        collationMatchesDefault,
                                        liteParsedPipeline.getInvolvedNamespaces());
    // If any involved collection contains extended-range data, set a flag which individual
    // DocumentSource parsers can check.
    collections.forEach([&](const CollectionPtr& coll) {
        if (coll->getRequiresTimeseriesExtendedRangeSupport())
            expCtx->setRequiresTimeseriesExtendedRangeSupport(true);
    });

    auto requestForQueryStats = origRequest.has_value() ? *origRequest : request;
    expCtx->startExpressionCounters();
    auto pipeline = Pipeline::parse(requestForQueryStats.getPipeline(), expCtx);
    expCtx->stopExpressionCounters();

    // Register query stats with the pre-optimized pipeline. Exclude queries against collections
    // with encrypted fields. We still collect query stats on collection-less aggregations.
    bool hasEncryptedFields = ctx && ctx->getCollection() &&
        ctx->getCollection()->getCollectionOptions().encryptedFieldConfig;
    if (!hasEncryptedFields) {
        // If this is a query over a resolved view, we want to register query stats with the
        // original user-given request and pipeline, rather than the new request generated when
        // resolving the view.
        auto collectionType = determineCollectionType(
            ctx, resolvedView, liteParsedPipeline.hasChangeStream(), isCollectionless);
        NamespaceStringSet pipelineInvolvedNamespaces(liteParsedPipeline.getInvolvedNamespaces());
        query_stats::registerRequest(opCtx, origNss, [&]() {
            return std::make_unique<query_stats::AggKey>(requestForQueryStats,
                                                         *pipeline,
                                                         expCtx,
                                                         std::move(pipelineInvolvedNamespaces),
                                                         origNss,
                                                         collectionType);
        });
    }

    if (resolvedView.has_value()) {
        expCtx->startExpressionCounters();

        if (resolvedView->timeseries()) {
            // For timeseries, there may have been rewrites done on the raw BSON pipeline
            // during view resolution. We must parse the request's full resolved pipeline
            // which will account for those rewrites.
            // TODO SERVER-82101 Re-organize timeseries rewrites so timeseries can follow the
            // same pattern here as other views
            pipeline = Pipeline::parse(request.getPipeline(), expCtx);
        } else {
            // Parse the view pipeline, then stitch the user pipeline and view pipeline together
            // to build the total aggregation pipeline.
            auto userPipeline = std::move(pipeline);
            pipeline = Pipeline::parse(resolvedView->getPipeline(), expCtx);
            pipeline->appendPipeline(std::move(userPipeline));
        }

        expCtx->stopExpressionCounters();
    }

    // After parsing to detect if $$USER_ROLES is referenced in the query, set the value of
    // $$USER_ROLES for the aggregation.
    expCtx->setUserRoles();

    // Lookup the query settings and attach it to the 'expCtx'.
    // TODO: SERVER-73632 Remove feature flag for PM-635.
    // Query settings will only be looked up on mongos and therefore should be part of command body
    // on mongod if present.
    expCtx->setQuerySettings(
        query_settings::lookupQuerySettingsForAgg(expCtx,
                                                  request,
                                                  *pipeline,
                                                  liteParsedPipeline.getInvolvedNamespaces(),
                                                  request.getNamespace()));

    return pipeline;
}
// TODO SERVER-82228 refactor this file to use a class, which should allow removal of resolvedView
// and origRequest parameters
Status _runAggregate(OperationContext* opCtx,
                     AggregateCommandRequest& request,
                     const LiteParsedPipeline& liteParsedPipeline,
                     const DeferredCmd& cmdObj,
                     const PrivilegeVector& privileges,
                     rpc::ReplyBuilderInterface* result,
                     std::shared_ptr<ExternalDataSourceScopeGuard> externalDataSourceGuard,
                     boost::optional<const ResolvedView&> resolvedView,
                     boost::optional<const AggregateCommandRequest&> origRequest) {
    auto origNss = origRequest.has_value() ? origRequest->getNamespace() : request.getNamespace();
    // Perform some validations on the LiteParsedPipeline and request before continuing with the
    // aggregation command.
    performValidationChecks(opCtx, request, liteParsedPipeline);

    // If we are running a retryable write without shard key, check if the write was applied on this
    // shard, and if so, return early with an empty cursor with $_wasStatementExecuted
    // set to true. The isRetryableWrite() check here is to check that the client executed write was
    // a retryable write (which would've spawned an internal session for a retryable write to
    // execute the two phase write without shard key protocol), otherwise we skip the retryable
    // write check.
    auto isClusterQueryWithoutShardKeyCmd = request.getIsClusterQueryWithoutShardKeyCmd();
    if (opCtx->isRetryableWrite() && isClusterQueryWithoutShardKeyCmd) {
        auto stmtId = request.getStmtId();
        tassert(7058100, "StmtId must be set for a retryable write without shard key", stmtId);
        if (TransactionParticipant::get(opCtx).checkStatementExecuted(opCtx, *stmtId)) {
            CursorResponseBuilder::Options options;
            options.isInitialResponse = true;
            CursorResponseBuilder responseBuilder(result, options);
            boost::optional<CursorMetrics> metrics = request.getIncludeQueryStatsMetrics()
                ? boost::make_optional(CursorMetrics{})
                : boost::none;
            responseBuilder.setWasStatementExecuted(true);
            responseBuilder.done(
                0LL,
                origNss,
                std::move(metrics),
                SerializationContext::stateCommandReply(request.getSerializationContext()));
            return Status::OK();
        }
    }

    // For operations on views, this will be the underlying namespace.
    NamespaceString nss = request.getNamespace();

    // Determine if this aggregation has foreign collections that the execution subsystem needs
    // to be aware of.
    std::vector<NamespaceStringOrUUID> secondaryExecNssList =
        liteParsedPipeline.getForeignExecutionNamespaces();

    // The collation to use for this aggregation. boost::optional to distinguish between the case
    // where the collation has not yet been resolved, and where it has been resolved to nullptr.
    boost::optional<std::unique_ptr<CollatorInterface>> collatorToUse;
    ExpressionContext::CollationMatchesDefault collatorToUseMatchesDefault;

    // The UUID of the collection for the execution namespace of this aggregation.
    boost::optional<UUID> uuid;

    // If emplaced, AutoGetCollectionForReadCommand will throw if the sharding version for this
    // connection is out of date. If the namespace is a view, the lock will be released before
    // re-running the expanded aggregation.
    boost::optional<AutoGetCollectionForReadCommandMaybeLockFree> ctx;
    MultipleCollectionAccessor collections;

    // Going forward this operation must never ignore interrupt signals while waiting for lock
    // acquisition. This InterruptibleLockGuard will ensure that waiting for lock re-acquisition
    // after yielding will not ignore interrupt signals. This is necessary to avoid deadlocking with
    // replication rollback, which at the storage layer waits for all cursors to be closed under the
    // global MODE_X lock, after having sent interrupt signals to read operations. This operation
    // must never hold open storage cursors while ignoring interrupt.
    InterruptibleLockGuard interruptibleLockAcquisition(shard_role_details::getLocker(opCtx));

    auto catalog = CollectionCatalog::latest(opCtx);

    hangAfterAcquiringCollectionCatalog.executeIf(
        [&](const auto&) { hangAfterAcquiringCollectionCatalog.pauseWhileSet(); },
        [&](const BSONObj& data) { return nss.coll() == data["collection"].valueStringData(); });

    auto initContext = [&](auto_get_collection::ViewMode m) {
        auto initAutoGetCallback = [&]() {
            ctx.emplace(opCtx,
                        nss,
                        AutoGetCollection::Options{}.viewMode(m).secondaryNssOrUUIDs(
                            secondaryExecNssList.cbegin(), secondaryExecNssList.cend()),
                        AutoStatsTracker::LogMode::kUpdateTopAndCurOp);
        };
        bool anySecondaryCollectionNotLocal =
            intializeAutoGet(opCtx, nss, secondaryExecNssList, initAutoGetCallback);
        tassert(8322000,
                "Should have initialized AutoGet* after calling 'initializeAutoGet'",
                ctx.has_value());
        collections = MultipleCollectionAccessor(opCtx,
                                                 &ctx->getCollection(),
                                                 ctx->getNss(),
                                                 ctx->isAnySecondaryNamespaceAView() ||
                                                     anySecondaryCollectionNotLocal,
                                                 secondaryExecNssList);

        // Return the catalog that gets implicitly stashed during the collection acquisition
        // above, which also implicitly opened a storage snapshot. This catalog object can
        // be potentially different than the one obtained before and will be in sync with
        // the opened snapshot.
        return CollectionCatalog::get(opCtx);
    };

    auto resetContext = [&]() -> void {
        ctx.reset();
        collections.clear();
    };

    std::vector<unique_ptr<PlanExecutor, PlanExecutor::Deleter>> execs;
    boost::intrusive_ptr<ExpressionContext> expCtx;
    auto curOp = CurOp::get(opCtx);

    {
        // If we are in a transaction, check whether the parsed pipeline supports being in
        // a transaction and if the transaction's read concern is supported.
        if (opCtx->inMultiDocumentTransaction()) {
            liteParsedPipeline.assertSupportsMultiDocumentTransaction(request.getExplain());
            liteParsedPipeline.assertSupportsReadConcern(opCtx, request.getExplain());
        }

        const auto& pipelineInvolvedNamespaces = liteParsedPipeline.getInvolvedNamespaces();

        // If this is a collectionless aggregation, we won't create 'ctx' but will still need an
        // AutoStatsTracker to record CurOp and Top entries.
        boost::optional<AutoStatsTracker> statsTracker;

        // If this is a change stream, perform special checks and change the execution namespace.
        if (liteParsedPipeline.hasChangeStream()) {
            uassert(4928900,
                    str::stream() << AggregateCommandRequest::kCollectionUUIDFieldName
                                  << " is not supported for a change stream",
                    !request.getCollectionUUID());

            // Replace the execution namespace with the oplog.
            nss = NamespaceString::kRsOplogNamespace;

            // In case of serverless the change stream will be opened on the change collection.
            const bool isServerless = change_stream_serverless_helpers::isServerlessEnvironment();
            if (isServerless) {
                const auto tenantId =
                    change_stream_serverless_helpers::resolveTenantId(origNss.tenantId());

                uassert(ErrorCodes::BadValue,
                        "Change streams cannot be used without tenant id",
                        tenantId);
                nss = NamespaceString::makeChangeCollectionNSS(tenantId);
            }

            // Assert that a change stream on the config server is always opened on the oplog.
            tassert(6763400,
                    str::stream() << "Change stream was unexpectedly opened on the namespace: "
                                  << nss.toStringForErrorMsg() << " in the config server",
                    !serverGlobalParams.clusterRole.has(ClusterRole::ConfigServer) ||
                        nss.isOplog());

            // Upgrade and wait for read concern if necessary.
            _adjustChangeStreamReadConcern(opCtx);

            // Obtain collection locks on the execution namespace; that is, the oplog.
            catalog = initContext(auto_get_collection::ViewMode::kViewsForbidden);

            // Raise an error if 'origNss' is a view. We do not need to check this if we are opening
            // a stream on an entire db or across the cluster.
            if (!origNss.isCollectionlessAggregateNS()) {
                auto view = catalog->lookupView(opCtx, origNss);
                uassert(ErrorCodes::CommandNotSupportedOnView,
                        str::stream() << "Cannot run aggregation on timeseries with namespace "
                                      << origNss.toStringForErrorMsg(),
                        !view || !view->timeseries());
                uassert(ErrorCodes::CommandNotSupportedOnView,
                        str::stream() << "Namespace " << origNss.toStringForErrorMsg()
                                      << " is a view, not a collection",
                        !view);
            }

            // If the user specified an explicit collation, adopt it; otherwise, use the simple
            // collation. We do not inherit the collection's default collation or UUID, since
            // the stream may be resuming from a point before the current UUID existed.
            auto [collator, match] = resolveCollator(
                opCtx, request.getCollation().get_value_or(BSONObj()), CollectionPtr());
            collatorToUse.emplace(std::move(collator));
            collatorToUseMatchesDefault = match;

            uassert(ErrorCodes::ChangeStreamNotEnabled,
                    "Change streams must be enabled before being used",
                    !isServerless ||
                        change_stream_serverless_helpers::isChangeStreamEnabled(opCtx,
                                                                                *nss.tenantId()));
        } else if (nss.isCollectionlessAggregateNS() && pipelineInvolvedNamespaces.empty()) {
            uassert(4928901,
                    str::stream() << AggregateCommandRequest::kCollectionUUIDFieldName
                                  << " is not supported for a collectionless aggregation",
                    !request.getCollectionUUID());

            // If this is a collectionless agg with no foreign namespaces, don't acquire any locks.
            statsTracker.emplace(opCtx,
                                 nss,
                                 Top::LockType::NotLocked,
                                 AutoStatsTracker::LogMode::kUpdateTopAndCurOp,
                                 catalog->getDatabaseProfileLevel(nss.dbName()));
            auto [collator, match] = resolveCollator(
                opCtx, request.getCollation().get_value_or(BSONObj()), CollectionPtr());
            collatorToUse.emplace(std::move(collator));
            collatorToUseMatchesDefault = match;
            tassert(6235101,
                    "A collection-less aggregate should not take any locks",
                    ctx == boost::none);
        } else {
            // This is a regular aggregation. Lock the collection or view.
            catalog = initContext(auto_get_collection::ViewMode::kViewsPermitted);
            auto [collator, match] = resolveCollator(opCtx,
                                                     request.getCollation().get_value_or(BSONObj()),
                                                     collections.getMainCollection());
            collatorToUse.emplace(std::move(collator));
            collatorToUseMatchesDefault = match;
            if (collections.hasMainCollection()) {
                uuid = collections.getMainCollection()->uuid();
            }
        }
        if (request.getResumeAfter()) {
            uassert(ErrorCodes::InvalidPipelineOperator,
                    "$_resumeAfter is not supported on view",
                    !ctx->getView());
            const auto& collection = ctx->getCollection();
            const bool isClusteredCollection = collection && collection->isClustered();
            uassertStatusOK(query_request_helper::validateResumeAfter(
                opCtx, *request.getResumeAfter(), isClusteredCollection));
        }

        // If collectionUUID was provided, verify the collection exists and has the expected UUID.
        checkCollectionUUIDMismatch(
            opCtx, nss, collections.getMainCollection(), request.getCollectionUUID());

        // If this is a view, resolve it by finding the underlying collection and stitching view
        // pipelines and this request's pipeline together. We then release our locks before
        // recursively calling runAggregate(), which will re-acquire locks on the underlying
        // collection.  (The lock must be released because recursively acquiring locks on the
        // database will prohibit yielding.)
        // We do not need to expand the view pipeline when there is a $collStats stage, as
        // $collStats is supported on a view namespace. For a time-series collection, however, the
        // view is abstracted out for the users, so we needed to resolve the namespace to get the
        // underlying bucket collection.
        if (ctx && ctx->getView() &&
            (!liteParsedPipeline.startsWithCollStats() || ctx->getView()->timeseries())) {
            return runAggregateOnView(opCtx,
                                      origNss,
                                      request,
                                      collections,
                                      std::move(collatorToUse),
                                      ctx->getView(),
                                      catalog,
                                      privileges,
                                      result,
                                      resetContext);
        }

        invariant(collatorToUse);


        std::unique_ptr<Pipeline, PipelineDeleter> pipeline =
            parsePipelineAndRegisterQueryStats(opCtx,
                                               origNss,
                                               request,
                                               ctx,
                                               std::move(*collatorToUse),
                                               uuid,
                                               collatorToUseMatchesDefault,
                                               collections,
                                               liteParsedPipeline,
                                               nss.isCollectionlessAggregateNS(),
                                               resolvedView,
                                               origRequest);
        expCtx = pipeline->getContext();

        CurOp::get(opCtx)->beginQueryPlanningTimer();

        // This prevents opening a new change stream in the critical section of a serverless shard
        // split or merge operation to prevent resuming on the recipient with a resume token higher
        // than that operation's blockTimestamp.
        //
        // If we do this check before picking a startTime for a change stream then the primary could
        // go into a blocking state between the check and getting the timestamp resulting in a
        // startTime greater than blockTimestamp. Therefore we must do this check here, after the
        // pipeline has been parsed and startTime has been initialized.
        if (liteParsedPipeline.hasChangeStream()) {
            tenant_migration_access_blocker::assertCanOpenChangeStream(expCtx->opCtx, nss.dbName());
        }

        if (!request.getAllowDiskUse().value_or(true)) {
            allowDiskUseFalseCounter.increment();
        }

        // Check that the view's collation matches the collation of any views involved in the
        // pipeline.
        if (!pipelineInvolvedNamespaces.empty()) {
            auto pipelineCollationStatus =
                collatorCompatibleWithPipeline(opCtx, expCtx->getCollator(), liteParsedPipeline);
            if (!pipelineCollationStatus.isOK()) {
                return pipelineCollationStatus;
            }
        }

        // If the aggregate command supports encrypted collections, do rewrites of the pipeline to
        // support querying against encrypted fields.
        if (shouldDoFLERewrite(request)) {
            {
                stdx::lock_guard<Client> lk(*opCtx->getClient());
                CurOp::get(opCtx)->setShouldOmitDiagnosticInformation_inlock(lk, true);
            }

            if (!request.getEncryptionInformation()->getCrudProcessed().value_or(false)) {
                pipeline = processFLEPipelineD(
                    opCtx, nss, request.getEncryptionInformation().value(), std::move(pipeline));
                request.getEncryptionInformation()->setCrudProcessed(true);
            }
        }

        pipeline->optimizePipeline();

        constexpr bool alreadyOptimized = true;
        pipeline->validateCommon(alreadyOptimized);

        if (auto sampleId = analyze_shard_key::getOrGenerateSampleId(
                opCtx,
                expCtx->ns,
                analyze_shard_key::SampledCommandNameEnum::kAggregate,
                request)) {
            analyze_shard_key::QueryAnalysisWriter::get(opCtx)
                ->addAggregateQuery(*sampleId,
                                    expCtx->ns,
                                    pipeline->getInitialQuery(),
                                    expCtx->getCollatorBSON(),
                                    request.getLet())
                .getAsync([](auto) {});
        }

        auto bonsaiEligibility =
            determineBonsaiEligibility(opCtx, collections.getMainCollection(), request, *pipeline);
        const bool bonsaiEligible = isEligibleForBonsaiUnderFrameworkControl(
            opCtx, request.getExplain().has_value(), bonsaiEligibility);

        bool bonsaiExecSuccess = true;
        if (bonsaiEligible) {
            uassert(6624344,
                    "Exchanging is not supported in the Cascades optimizer",
                    !request.getExchange().has_value());
            uassert(ErrorCodes::InternalErrorNotSupported,
                    "runtimeConstants unsupported in CQF",
                    !request.getLegacyRuntimeConstants());
            uassert(ErrorCodes::InternalErrorNotSupported,
                    "$_requestReshardingResumeToken in CQF",
                    !request.getRequestReshardingResumeToken());
            uassert(ErrorCodes::InternalErrorNotSupported,
                    "collation unsupported in CQF",
                    !request.getCollation() || request.getCollation()->isEmpty() ||
                        SimpleBSONObjComparator::kInstance.evaluate(*request.getCollation() ==
                                                                    CollationSpec::kSimpleSpec));

            optimizer::QueryHints queryHints = getHintsFromQueryKnobs();
            const bool fastIndexNullHandling = queryHints._fastIndexNullHandling;
            auto timeBegin = Date_t::now();
            auto maybeExec = [&] {
                // If the query is eligible for a fast path, use the fast path plan instead of
                // invoking the optimizer.
                if (auto fastPathExec = optimizer::fast_path::tryGetSBEExecutorViaFastPath(
                        opCtx,
                        expCtx,
                        nss,
                        collections,
                        request.getExplain().has_value(),
                        request.getHint(),
                        pipeline.get())) {
                    return fastPathExec;
                }

                // Checks that the pipeline is M2/M4-eligible in Bonsai. An eligible pipeline has
                // (1) a $match first stage, and (2) an optional $project second stage. All other
                // stages are unsupported.
                auto fullyEligible = [](auto& sources) {
                    // Sources cannot contain more than two stages.
                    if (sources.size() > 2) {
                        return false;
                    }
                    const auto& firstStageItr = sources.begin();
                    // If optional second stage exists, must be a projection stage.
                    if (auto secondStageItr = std::next(firstStageItr);
                        secondStageItr != sources.end()) {
                        return secondStageItr->get()->getSourceName() ==
                            DocumentSourceProject::kStageName;
                    }
                    return true;
                };

                if (internalCascadesOptimizerEnableParameterization.load() &&
                    bonsaiEligibility.isFullyEligible() && pipeline->canParameterize() &&
                    fullyEligible(pipeline->getSources())) {
                    pipeline->parameterize();
                }

                return getSBEExecutorViaCascadesOptimizer(opCtx,
                                                          expCtx,
                                                          nss,
                                                          collections,
                                                          std::move(queryHints),
                                                          request.getHint(),
                                                          bonsaiEligibility,
                                                          pipeline.get());
            }();
            if (maybeExec) {
                // Pass ownership of the pipeline to the executor. This is done to allow binding of
                // parameters to use views onto the constants living in the MatchExpression (in the
                // DocumentSourceMatch in the Pipeline), so we can avoid copying them into the SBE
                // runtime environment. We must ensure that the MatchExpression lives at least as
                // long as the executor.
                execs.emplace_back(uassertStatusOK(makeExecFromParams(
                    nullptr, std::move(pipeline), collections, std::move(*maybeExec))));
            } else {
                // If we had an optimization failure, only error if we're not in tryBonsai.
                bonsaiExecSuccess = false;
                auto queryControl = QueryKnobConfiguration::decoration(opCtx)
                                        .getInternalQueryFrameworkControlForOp();
                tassert(7319401,
                        "Optimization failed either without tryBonsai set, or without a hint.",
                        queryControl == QueryFrameworkControlEnum::kTryBonsai &&
                            request.getHint() && !request.getHint()->isEmpty() &&
                            !fastIndexNullHandling);
            }

            auto elapsed =
                (Date_t::now().toMillisSinceEpoch() - timeBegin.toMillisSinceEpoch()) / 1000.0;
            OPTIMIZER_DEBUG_LOG(
                6264804, 5, "Cascades optimization time elapsed", "time"_attr = elapsed);
        }

        if (!bonsaiEligible || !bonsaiExecSuccess) {
            execs = createLegacyExecutor(std::move(pipeline),
                                         liteParsedPipeline,
                                         nss,
                                         collections,
                                         request,
                                         curOp,
                                         resetContext);
        }
        tassert(6624353, "No executors", !execs.empty());


        {
            auto planSummary = execs[0]->getPlanExplainer().getPlanSummary();
            stdx::lock_guard<Client> lk(*opCtx->getClient());
            curOp->setPlanSummary_inlock(std::move(planSummary));
            curOp->debug().queryFramework = execs[0]->getQueryFramework();
        }
    }

    // Having released the collection lock, we can now begin to fetch results from the pipeline. If
    // the documents in the result exceed the batch size, a cursor will be created. This cursor owns
    // no collection state, and thus we register it with the global cursor manager. The global
    // cursor manager does not deliver invalidations or kill notifications; the underlying
    // PlanExecutor(s) used by the pipeline will be receiving invalidations and kill notifications
    // themselves, not the cursor we create here.
    hangAfterCreatingAggregationPlan.executeIf(
        [](const auto&) { hangAfterCreatingAggregationPlan.pauseWhileSet(); },
        [&](const BSONObj& data) { return uuid && UUID::parse(data["uuid"]) == *uuid; });
    // Report usage statistics for each stage in the pipeline.
    liteParsedPipeline.tickGlobalStageCounters();

    // If both explain and cursor are specified, explain wins.
    if (expCtx->explain) {
        auto explainExecutor = execs[0].get();
        auto bodyBuilder = result->getBodyBuilder();
        if (auto pipelineExec = dynamic_cast<PlanExecutorPipeline*>(explainExecutor)) {
            Explain::explainPipeline(pipelineExec,
                                     true /* executePipeline */,
                                     *(expCtx->explain),
                                     *cmdObj,
                                     &bodyBuilder);
        } else {
            invariant(explainExecutor->getOpCtx() == opCtx);
            // The explainStages() function for a non-pipeline executor may need to execute the plan
            // to collect statistics. If the PlanExecutor uses kLockExternally policy, the
            // appropriate collection lock must be already held. Make sure it has not been released
            // yet.
            invariant(ctx);
            Explain::explainStages(
                explainExecutor,
                collections,
                *(expCtx->explain),
                BSON("optimizedPipeline" << true),
                SerializationContext::stateCommandReply(request.getSerializationContext()),
                *cmdObj,
                &bodyBuilder);
        }
        collectQueryStatsMongod(opCtx, std::move(curOp->debug().queryStatsInfo.key));
    } else {
        auto maybePinnedCursor = executeUntilFirstBatch(opCtx,
                                                        expCtx,
                                                        request,
                                                        cmdObj,
                                                        privileges,
                                                        origNss,
                                                        externalDataSourceGuard,
                                                        execs,
                                                        result);

        // For an optimized away pipeline, signal the cache that a query operation has completed.
        // For normal pipelines this is done in DocumentSourceCursor.
        if (ctx) {
            // Due to yielding, the collection pointers saved in MultipleCollectionAccessor might
            // have become invalid. We will need to refresh them here.

            // Stash the old value of 'isAnySecondaryNamespaceAViewOrNotFullyLocal' before
            // refreshing. This is ok because we expect that our view of the collection should not
            // have changed while holding 'ctx'.
            auto isAnySecondaryNamespaceAViewOrNotFullyLocal =
                collections.isAnySecondaryNamespaceAViewOrNotFullyLocal();
            collections = MultipleCollectionAccessor(opCtx,
                                                     &ctx->getCollection(),
                                                     ctx->getNss(),
                                                     isAnySecondaryNamespaceAViewOrNotFullyLocal,
                                                     secondaryExecNssList);

            auto exec =
                maybePinnedCursor ? maybePinnedCursor->getCursor()->getExecutor() : execs[0].get();
            const auto& planExplainer = exec->getPlanExplainer();
            if (const auto& coll = ctx->getCollection()) {
                CollectionQueryInfo::get(coll).notifyOfQuery(coll, curOp->debug());
            }
            // For SBE pushed down pipelines, we may need to report stats saved for secondary
            // collections separately.
            for (const auto& [secondaryNss, coll] : collections.getSecondaryCollections()) {
                if (coll) {
                    PlanSummaryStats secondaryStats;
                    planExplainer.getSecondarySummaryStats(secondaryNss, &secondaryStats);
                    CollectionQueryInfo::get(coll).notifyOfQuery(opCtx, coll, secondaryStats);
                }
            }
        }
    }

    // The aggregation pipeline may change the namespace of the curop and we need to set it back to
    // the original namespace to correctly report command stats. One example when the namespace can
    // be changed is when the pipeline contains an $out stage, which executes an internal command to
    // create a temp collection, changing the curop namespace to the name of this temp collection.
    {
        stdx::lock_guard<Client> lk(*opCtx->getClient());
        curOp->setNS_inlock(origNss);
    }

    return Status::OK();
}

}  // namespace

Status runAggregate(OperationContext* opCtx,
                    AggregateCommandRequest& request,
                    const BSONObj& cmdObj,
                    const PrivilegeVector& privileges,
                    rpc::ReplyBuilderInterface* result) {
    return _runAggregate(opCtx,
                         request,
                         {request},
                         DeferredCmd(cmdObj),
                         privileges,
                         result,
                         nullptr,
                         boost::none,
                         boost::none);
}

Status runAggregate(
    OperationContext* opCtx,
    AggregateCommandRequest& request,
    const LiteParsedPipeline& liteParsedPipeline,
    const BSONObj& cmdObj,
    const PrivilegeVector& privileges,
    rpc::ReplyBuilderInterface* result,
    const std::vector<std::pair<NamespaceString, std::vector<ExternalDataSourceInfo>>>&
        usedExternalDataSources) {
    // Create virtual collections and drop them when aggregate command is done.
    // If a cursor is registered, the ExternalDataSourceScopeGuard will be stored in the cursor;
    // when the cursor is later destroyed, the scope guard will also be destroyed, and any virtual
    // collections will be dropped by the destructor of ExternalDataSourceScopeGuard.
    // We create this scope guard prior to taking locks in _runAggregate so that, if no cursor is
    // registered, the virtual collections will be dropped after releasing our read locks, avoiding
    // a lock upgrade.
    auto extDataSrcGuard = usedExternalDataSources.size() > 0
        ? std::make_shared<ExternalDataSourceScopeGuard>(opCtx, usedExternalDataSources)
        : nullptr;

    return _runAggregate(opCtx,
                         request,
                         liteParsedPipeline,
                         DeferredCmd(cmdObj),
                         privileges,
                         result,
                         extDataSrcGuard,
                         boost::none,
                         boost::none);
}
}  // namespace mongo
