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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/s/query/cluster_aggregate.h"

#include <boost/intrusive_ptr.hpp>

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/curop.h"
#include "mongo/db/logical_clock.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_out.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/mongos_process_interface.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/pipeline/sharded_agg_helpers.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/db/views/view.h"
#include "mongo/db/write_concern_options.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/rpc/op_msg_rpc_impls.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/multi_statement_transaction_requests_sender.h"
#include "mongo/s/query/cluster_aggregation_planner.h"
#include "mongo/s/query/cluster_client_cursor_impl.h"
#include "mongo/s/query/cluster_client_cursor_params.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/s/query/cluster_query_knobs_gen.h"
#include "mongo/s/query/document_source_merge_cursors.h"
#include "mongo/s/query/establish_cursors.h"
#include "mongo/s/query/owned_remote_cursor.h"
#include "mongo/s/query/router_stage_pipeline.h"
#include "mongo/s/query/store_possible_cursor.h"
#include "mongo/s/stale_exception.h"
#include "mongo/s/transaction_router.h"
#include "mongo/util/log.h"
#include "mongo/util/net/socket_utils.h"

namespace mongo {

constexpr unsigned ClusterAggregate::kMaxViewRetries;

namespace {

// "Resolve" involved namespaces into a map. We won't try to execute anything on a mongos, but we
// still have to populate this map so that any $lookups, etc. will be able to have a resolved view
// definition. It's okay that this is incorrect, we will repopulate the real namespace map on the
// mongod. Note that this function must be called before forwarding an aggregation command on an
// unsharded collection, in order to verify that the involved namespaces are allowed to be sharded.
auto resolveInvolvedNamespaces(stdx::unordered_set<NamespaceString> involvedNamespaces) {
    StringMap<ExpressionContext::ResolvedNamespace> resolvedNamespaces;
    for (auto&& nss : involvedNamespaces) {
        resolvedNamespaces.try_emplace(nss.coll(), nss, std::vector<BSONObj>{});
    }
    return resolvedNamespaces;
}

// Build an appropriate ExpressionContext for the pipeline. This helper instantiates an appropriate
// collator, creates a MongoProcessInterface for use by the pipeline's stages, and sets the
// collection UUID if provided.
boost::intrusive_ptr<ExpressionContext> makeExpressionContext(
    OperationContext* opCtx,
    const AggregationRequest& request,
    BSONObj collationObj,
    boost::optional<UUID> uuid,
    StringMap<ExpressionContext::ResolvedNamespace> resolvedNamespaces) {

    std::unique_ptr<CollatorInterface> collation;
    if (!collationObj.isEmpty()) {
        // This will be null if attempting to build an interface for the simple collator.
        collation = uassertStatusOK(
            CollatorFactoryInterface::get(opCtx->getServiceContext())->makeFromBSON(collationObj));
    }

    // Create the expression context, and set 'inMongos' to true. We explicitly do *not* set
    // mergeCtx->tempDir.
    auto mergeCtx = new ExpressionContext(opCtx,
                                          request,
                                          std::move(collation),
                                          std::make_shared<MongoSInterface>(),
                                          std::move(resolvedNamespaces),
                                          uuid);

    // Keep the backing collation object on the context up to date with the resolved collator.
    mergeCtx->collation = collationObj;

    mergeCtx->inMongos = true;
    return mergeCtx;
}

void appendEmptyResultSetWithStatus(OperationContext* opCtx,
                                    const NamespaceString& nss,
                                    Status status,
                                    BSONObjBuilder* result) {
    // Rewrite ShardNotFound as NamespaceNotFound so that appendEmptyResultSet swallows it.
    if (status == ErrorCodes::ShardNotFound) {
        status = {ErrorCodes::NamespaceNotFound, status.reason()};
    }
    appendEmptyResultSet(opCtx, *result, status, nss.ns());
}

}  // namespace

Status ClusterAggregate::runAggregate(OperationContext* opCtx,
                                      const Namespaces& namespaces,
                                      const AggregationRequest& request,
                                      const PrivilegeVector& privileges,
                                      BSONObjBuilder* result) {
    uassert(51028, "Cannot specify exchange option to a mongos", !request.getExchangeSpec());
    uassert(51143,
            "Cannot specify runtime constants option to a mongos",
            !request.getRuntimeConstants());
    uassert(51089,
            str::stream() << "Internal parameter(s) [" << AggregationRequest::kNeedsMergeName
                          << ", " << AggregationRequest::kFromMongosName
                          << "] cannot be set to 'true' when sent to mongos",
            !request.needsMerge() && !request.isFromMongos());

    const auto isSharded = [](OperationContext* opCtx, const NamespaceString& nss) -> bool {
        const auto resolvedNsRoutingInfo =
            uassertStatusOK(getCollectionRoutingInfoForTxnCmd(opCtx, nss));
        return resolvedNsRoutingInfo.cm().get();
    };

    LiteParsedPipeline litePipe(request);
    litePipe.verifyIsSupported(
        opCtx, isSharded, request.getExplain(), serverGlobalParams.enableMajorityReadConcern);
    auto hasChangeStream = litePipe.hasChangeStream();
    auto involvedNamespaces = litePipe.getInvolvedNamespaces();

    const auto pipelineBuilder = [&](boost::optional<CachedCollectionRoutingInfo> routingInfo) {
        // Populate the collection UUID and the appropriate collation to use.
        auto [collationObj, uuid] = [&]() -> std::pair<BSONObj, boost::optional<UUID>> {
            // If this is a change stream, take the user-defined collation if one exists, or an
            // empty BSONObj otherwise. Change streams never inherit the collection's default
            // collation, and since collectionless aggregations generally run on the 'admin'
            // database, the standard logic would attempt to resolve its non-existent UUID and
            // collation by sending a specious 'listCollections' command to the config servers.
            if (hasChangeStream) {
                return {request.getCollation(), boost::none};
            }

            return sharded_agg_helpers::getCollationAndUUID(
                routingInfo, namespaces.executionNss, request.getCollation());
        }();

        // Build an ExpressionContext for the pipeline. This instantiates an appropriate collator,
        // resolves all involved namespaces, and creates a shared MongoProcessInterface for use by
        // the pipeline's stages.
        auto expCtx = makeExpressionContext(
            opCtx, request, collationObj, uuid, resolveInvolvedNamespaces(involvedNamespaces));

        // Parse and optimize the full pipeline.
        auto pipeline = uassertStatusOK(Pipeline::parse(request.getPipeline(), expCtx));
        pipeline->optimizePipeline();
        return pipeline;
    };

    auto targetingStatus =
        sharded_agg_helpers::AggregationTargeter::make(opCtx,
                                                       namespaces.executionNss,
                                                       pipelineBuilder,
                                                       involvedNamespaces,
                                                       hasChangeStream,
                                                       litePipe.allowedToPassthroughFromMongos());
    if (!targetingStatus.isOK()) {
        appendEmptyResultSetWithStatus(
            opCtx, namespaces.requestedNss, targetingStatus.getStatus(), result);
        return Status::OK();
    }

    auto targeter = std::move(targetingStatus.getValue());
    switch (targeter.policy) {
        case sharded_agg_helpers::AggregationTargeter::TargetingPolicy::kPassthrough: {
            // A pipeline with $changeStream should never be allowed to passthrough.
            invariant(!hasChangeStream);
            return sharded_agg_helpers::runPipelineOnPrimaryShard(opCtx,
                                                                  namespaces,
                                                                  targeter.routingInfo->db(),
                                                                  request.getExplain(),
                                                                  request.serializeToCommandObj(),
                                                                  privileges,
                                                                  result);
        }

        case sharded_agg_helpers::AggregationTargeter::TargetingPolicy::kMongosRequired: {
            auto expCtx = targeter.pipeline->getContext();
            // If this is an explain write the explain output and return.
            if (expCtx->explain) {
                *result << "splitPipeline" << BSONNULL << "mongos"
                        << Document{
                               {"host", getHostNameCachedAndPort()},
                               {"stages", targeter.pipeline->writeExplainOps(*expCtx->explain)}};
                return Status::OK();
            }

            return sharded_agg_helpers::runPipelineOnMongoS(namespaces,
                                                            request.getBatchSize(),
                                                            std::move(targeter.pipeline),
                                                            result,
                                                            privileges);
        }

        case sharded_agg_helpers::AggregationTargeter::TargetingPolicy::kAnyShard: {
            return sharded_agg_helpers::dispatchPipelineAndMerge(opCtx,
                                                                 std::move(targeter),
                                                                 request.serializeToCommandObj(),
                                                                 request.getBatchSize(),
                                                                 namespaces,
                                                                 privileges,
                                                                 result,
                                                                 hasChangeStream);
        }
    }

    MONGO_UNREACHABLE;
}

Status ClusterAggregate::retryOnViewError(OperationContext* opCtx,
                                          const AggregationRequest& request,
                                          const ResolvedView& resolvedView,
                                          const NamespaceString& requestedNss,
                                          const PrivilegeVector& privileges,
                                          BSONObjBuilder* result,
                                          unsigned numberRetries) {
    if (numberRetries >= kMaxViewRetries) {
        return Status(ErrorCodes::InternalError,
                      "Failed to resolve view after max number of retries.");
    }

    auto resolvedAggRequest = resolvedView.asExpandedViewAggregation(request);
    result->resetToEmpty();

    if (auto txnRouter = TransactionRouter::get(opCtx)) {
        txnRouter.onViewResolutionError(opCtx, requestedNss);
    }

    // We pass both the underlying collection namespace and the view namespace here. The
    // underlying collection namespace is used to execute the aggregation on mongoD. Any cursor
    // returned will be registered under the view namespace so that subsequent getMore and
    // killCursors calls against the view have access.
    Namespaces nsStruct;
    nsStruct.requestedNss = requestedNss;
    nsStruct.executionNss = resolvedView.getNamespace();

    auto status =
        ClusterAggregate::runAggregate(opCtx, nsStruct, resolvedAggRequest, privileges, result);

    // If the underlying namespace was changed to a view during retry, then re-run the aggregation
    // on the new resolved namespace.
    if (status.extraInfo<ResolvedView>()) {
        return ClusterAggregate::retryOnViewError(opCtx,
                                                  resolvedAggRequest,
                                                  *status.extraInfo<ResolvedView>(),
                                                  requestedNss,
                                                  privileges,
                                                  result,
                                                  numberRetries + 1);
    }

    return status;
}

}  // namespace mongo
