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
#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include "sharded_agg_helpers.h"

#include "mongo/db/curop.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/pipeline/document_source_out.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/cluster_commands_helpers.h"
#include "mongo/s/query/cluster_query_knobs_gen.h"
#include "mongo/s/query/document_source_merge_cursors.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/log.h"

namespace mongo {
namespace sharded_agg_helpers {

MONGO_FAIL_POINT_DEFINE(clusterAggregateHangBeforeEstablishingShardCursors);

/**
 * Given a document representing an aggregation command such as
 * {aggregate: "myCollection", pipeline: [], ...},
 *
 * produces the corresponding explain command:
 * {explain: {aggregate: "myCollection", pipline: [], ...}, $queryOptions: {...}, verbosity: ...}
 */
Document wrapAggAsExplain(Document aggregateCommand, ExplainOptions::Verbosity verbosity) {
    MutableDocument explainCommandBuilder;
    explainCommandBuilder["explain"] = Value(aggregateCommand);
    // Downstream host targeting code expects queryOptions at the top level of the command object.
    explainCommandBuilder[QueryRequest::kUnwrappedReadPrefField] =
        Value(aggregateCommand[QueryRequest::kUnwrappedReadPrefField]);

    // readConcern needs to be promoted to the top-level of the request.
    explainCommandBuilder[repl::ReadConcernArgs::kReadConcernFieldName] =
        Value(aggregateCommand[repl::ReadConcernArgs::kReadConcernFieldName]);

    // Add explain command options.
    for (auto&& explainOption : ExplainOptions::toBSON(verbosity)) {
        explainCommandBuilder[explainOption.fieldNameStringData()] = Value(explainOption);
    }

    return explainCommandBuilder.freeze();
}

BSONObj createPassthroughCommandForShard(OperationContext* opCtx,
                                         const AggregationRequest& request,
                                         const boost::optional<ShardId>& shardId,
                                         Pipeline* pipeline,
                                         BSONObj collationObj) {
    // Create the command for the shards.
    MutableDocument targetedCmd(request.serializeToCommandObj());
    if (pipeline) {
        targetedCmd[AggregationRequest::kPipelineName] = Value(pipeline->serialize());
    }

    return genericTransformForShards(std::move(targetedCmd), opCtx, shardId, request, collationObj);
}

BSONObj genericTransformForShards(MutableDocument&& cmdForShards,
                                  OperationContext* opCtx,
                                  const boost::optional<ShardId>& shardId,
                                  const AggregationRequest& request,
                                  BSONObj collationObj) {
    cmdForShards[AggregationRequest::kFromMongosName] = Value(true);
    // If this is a request for an aggregation explain, then we must wrap the aggregate inside an
    // explain command.
    if (auto explainVerbosity = request.getExplain()) {
        cmdForShards.reset(wrapAggAsExplain(cmdForShards.freeze(), *explainVerbosity));
    }

    if (!collationObj.isEmpty()) {
        cmdForShards[AggregationRequest::kCollationName] = Value(collationObj);
    }

    if (opCtx->getTxnNumber()) {
        invariant(cmdForShards.peek()[OperationSessionInfo::kTxnNumberFieldName].missing(),
                  str::stream() << "Command for shards unexpectedly had the "
                                << OperationSessionInfo::kTxnNumberFieldName
                                << " field set: "
                                << cmdForShards.peek().toString());
        cmdForShards[OperationSessionInfo::kTxnNumberFieldName] =
            Value(static_cast<long long>(*opCtx->getTxnNumber()));
    }

    // agg creates temp collection and should handle implicit create separately.
    return appendAllowImplicitCreate(cmdForShards.freeze().toBson(), true);
}

StatusWith<CachedCollectionRoutingInfo> getExecutionNsRoutingInfo(OperationContext* opCtx,
                                                                  const NamespaceString& execNss) {
    // First, verify that there are shards present in the cluster. If not, then we return the
    // stronger 'ShardNotFound' error rather than 'NamespaceNotFound'. We must do this because
    // $changeStream aggregations ignore NamespaceNotFound in order to allow streams to be opened on
    // a collection before its enclosing database is created. However, if there are no shards
    // present, then $changeStream should immediately return an empty cursor just as other
    // aggregations do when the database does not exist.
    std::vector<ShardId> shardIds;
    Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx, &shardIds);
    if (shardIds.size() == 0) {
        return {ErrorCodes::ShardNotFound, "No shards are present in the cluster"};
    }

    // This call to getCollectionRoutingInfoForTxnCmd will return !OK if the database does not
    // exist.
    return getCollectionRoutingInfoForTxnCmd(opCtx, execNss);
}

Shard::RetryPolicy getDesiredRetryPolicy(const AggregationRequest& req) {
    // The idempotent retry policy will retry even for writeConcern failures, so only set it if the
    // pipeline does not support writeConcern.
    if (req.getWriteConcern()) {
        return Shard::RetryPolicy::kNotIdempotent;
    }
    return Shard::RetryPolicy::kIdempotent;
}

bool mustRunOnAllShards(const NamespaceString& nss, const LiteParsedPipeline& litePipe) {
    // The following aggregations must be routed to all shards:
    // - Any collectionless aggregation, such as non-localOps $currentOp.
    // - Any aggregation which begins with a $changeStream stage.
    return nss.isCollectionlessAggregateNS() || litePipe.hasChangeStream();
}
BSONObj createCommandForTargetedShards(
    OperationContext* opCtx,
    const AggregationRequest& request,
    const LiteParsedPipeline& litePipe,
    const cluster_aggregation_planner::SplitPipeline& splitPipeline,
    const BSONObj collationObj,
    const boost::optional<cluster_aggregation_planner::ShardedExchangePolicy> exchangeSpec,
    bool needsMerge) {

    // Create the command for the shards.
    MutableDocument targetedCmd(request.serializeToCommandObj());
    // If we've parsed a pipeline on mongos, always override the pipeline, in case parsing it
    // has defaulted any arguments or otherwise changed the spec. For example, $listSessions may
    // have detected a logged in user and appended that user name to the $listSessions spec to
    // send to the shards.
    targetedCmd[AggregationRequest::kPipelineName] =
        Value(splitPipeline.shardsPipeline->serialize());

    // When running on many shards with the exchange we may not need merging.
    if (needsMerge) {
        targetedCmd[AggregationRequest::kNeedsMergeName] = Value(true);

        // If this is a change stream aggregation, set the 'mergeByPBRT' flag on the command. This
        // notifies the shards that the mongoS is capable of merging streams based on resume token.
        // TODO SERVER-38539: the 'mergeByPBRT' flag is no longer necessary in 4.4.
        targetedCmd[AggregationRequest::kMergeByPBRTName] = Value(litePipe.hasChangeStream());

        // If there aren't any stages like $out in the pipeline being sent to the shards, remove the
        // write concern. The write concern should only be applied when there are writes performed
        // to avoid mistakenly waiting for writes which didn't happen.
        const auto& shardsPipe = splitPipeline.shardsPipeline->getSources();
        if (!std::any_of(shardsPipe.begin(), shardsPipe.end(), [](const auto& stage) {
                return stage->constraints().writesPersistentData();
            })) {
            targetedCmd[WriteConcernOptions::kWriteConcernField] = Value();
        }
    }

    targetedCmd[AggregationRequest::kCursorName] =
        Value(DOC(AggregationRequest::kBatchSizeName << 0));

    targetedCmd[AggregationRequest::kExchangeName] =
        exchangeSpec ? Value(exchangeSpec->exchangeSpec.toBSON()) : Value();

    return genericTransformForShards(
        std::move(targetedCmd), opCtx, boost::none, request, collationObj);
}

/**
 * Targets shards for the pipeline and returns a struct with the remote cursors or results, and
 * the pipeline that will need to be executed to merge the results from the remotes. If a stale
 * shard version is encountered, refreshes the routing table and tries again.
 */
DispatchShardPipelineResults dispatchShardPipeline(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& executionNss,
    const AggregationRequest& aggRequest,
    const LiteParsedPipeline& litePipe,
    std::unique_ptr<Pipeline, PipelineDeleter> pipeline,
    BSONObj collationObj) {
    // The process is as follows:
    // - First, determine whether we need to target more than one shard. If so, we split the
    // pipeline; if not, we retain the existing pipeline.
    // - Call establishShardCursors to dispatch the aggregation to the targeted shards.
    // - Stale shard version errors are thrown up to the top-level handler, causing a retry on the
    // entire aggregation commmand.
    auto cursors = std::vector<RemoteCursor>();
    auto shardResults = std::vector<AsyncRequestsSender::Response>();
    auto opCtx = expCtx->opCtx;

    const bool needsPrimaryShardMerge =
        (pipeline->needsPrimaryShardMerger() || internalQueryAlwaysMergeOnPrimaryShard.load());

    const bool needsMongosMerge = pipeline->needsMongosMerger();

    const auto shardQuery = pipeline->getInitialQuery();

    auto executionNsRoutingInfoStatus = getExecutionNsRoutingInfo(opCtx, executionNss);

    // If this is a $changeStream, we swallow NamespaceNotFound exceptions and continue.
    // Otherwise, uassert on all exceptions here.
    if (!(litePipe.hasChangeStream() &&
          executionNsRoutingInfoStatus == ErrorCodes::NamespaceNotFound)) {
        uassertStatusOK(executionNsRoutingInfoStatus);
    }

    auto executionNsRoutingInfo = executionNsRoutingInfoStatus.isOK()
        ? std::move(executionNsRoutingInfoStatus.getValue())
        : boost::optional<CachedCollectionRoutingInfo>{};

    // Determine whether we can run the entire aggregation on a single shard.
    const bool mustRunOnAll = mustRunOnAllShards(executionNss, litePipe);
    std::set<ShardId> shardIds = getTargetedShards(
        opCtx, mustRunOnAll, executionNsRoutingInfo, shardQuery, aggRequest.getCollation());

    // Don't need to split the pipeline if we are only targeting a single shard, unless:
    // - There is a stage that needs to be run on the primary shard and the single target shard
    //   is not the primary.
    // - The pipeline contains one or more stages which must always merge on mongoS.
    const bool needsSplit = (shardIds.size() > 1u || needsMongosMerge ||
                             (needsPrimaryShardMerge && executionNsRoutingInfo &&
                              *(shardIds.begin()) != executionNsRoutingInfo->db().primaryId()));

    boost::optional<cluster_aggregation_planner::ShardedExchangePolicy> exchangeSpec;
    boost::optional<cluster_aggregation_planner::SplitPipeline> splitPipeline;

    if (needsSplit) {
        LOG(5) << "Splitting pipeline: "
               << "targeting = " << shardIds.size()
               << " shards, needsMongosMerge = " << needsMongosMerge
               << ", needsPrimaryShardMerge = " << needsPrimaryShardMerge;
        splitPipeline = cluster_aggregation_planner::splitPipeline(std::move(pipeline));

        exchangeSpec = cluster_aggregation_planner::checkIfEligibleForExchange(
            opCtx, splitPipeline->mergePipeline.get());
    }

    // Generate the command object for the targeted shards.
    BSONObj targetedCommand = splitPipeline
        ? createCommandForTargetedShards(
              opCtx, aggRequest, litePipe, *splitPipeline, collationObj, exchangeSpec, true)
        : createPassthroughCommandForShard(
              opCtx, aggRequest, boost::none, pipeline.get(), collationObj);

    // Refresh the shard registry if we're targeting all shards.  We need the shard registry
    // to be at least as current as the logical time used when creating the command for
    // $changeStream to work reliably, so we do a "hard" reload.
    if (mustRunOnAll) {
        auto* shardRegistry = Grid::get(opCtx)->shardRegistry();
        if (!shardRegistry->reload(opCtx)) {
            shardRegistry->reload(opCtx);
        }
        // Rebuild the set of shards as the shard registry might have changed.
        shardIds = getTargetedShards(
            opCtx, mustRunOnAll, executionNsRoutingInfo, shardQuery, aggRequest.getCollation());
    }

    // Explain does not produce a cursor, so instead we scatter-gather commands to the shards.
    if (expCtx->explain) {
        if (mustRunOnAll) {
            // Some stages (such as $currentOp) need to be broadcast to all shards, and
            // should not participate in the shard version protocol.
            shardResults =
                scatterGatherUnversionedTargetAllShards(opCtx,
                                                        executionNss.db(),
                                                        targetedCommand,
                                                        ReadPreferenceSetting::get(opCtx),
                                                        Shard::RetryPolicy::kIdempotent);
        } else {
            // Aggregations on a real namespace should use the routing table to target
            // shards, and should participate in the shard version protocol.
            invariant(executionNsRoutingInfo);
            shardResults =
                scatterGatherVersionedTargetByRoutingTable(opCtx,
                                                           executionNss.db(),
                                                           executionNss,
                                                           *executionNsRoutingInfo,
                                                           targetedCommand,
                                                           ReadPreferenceSetting::get(opCtx),
                                                           Shard::RetryPolicy::kIdempotent,
                                                           shardQuery,
                                                           aggRequest.getCollation());
        }
    } else {
        cursors = establishShardCursors(opCtx,
                                        executionNss,
                                        litePipe,
                                        executionNsRoutingInfo,
                                        targetedCommand,
                                        aggRequest,
                                        ReadPreferenceSetting::get(opCtx),
                                        shardQuery);
        invariant(cursors.size() % shardIds.size() == 0,
                  str::stream() << "Number of cursors (" << cursors.size()
                                << ") is not a multiple of producers ("
                                << shardIds.size()
                                << ")");
    }

    // Convert remote cursors into a vector of "owned" cursors.
    std::vector<OwnedRemoteCursor> ownedCursors;
    for (auto&& cursor : cursors) {
        ownedCursors.emplace_back(OwnedRemoteCursor(opCtx, std::move(cursor), executionNss));
    }

    // Record the number of shards involved in the aggregation. If we are required to merge on
    // the primary shard, but the primary shard was not in the set of targeted shards, then we
    // must increment the number of involved shards.
    CurOp::get(opCtx)->debug().nShards =
        shardIds.size() + (needsPrimaryShardMerge && executionNsRoutingInfo &&
                           !shardIds.count(executionNsRoutingInfo->db().primaryId()));

    return DispatchShardPipelineResults{needsPrimaryShardMerge,
                                        std::move(ownedCursors),
                                        std::move(shardResults),
                                        std::move(splitPipeline),
                                        std::move(pipeline),
                                        targetedCommand,
                                        shardIds.size(),
                                        exchangeSpec};
}

std::set<ShardId> getTargetedShards(OperationContext* opCtx,
                                    bool mustRunOnAllShards,
                                    const boost::optional<CachedCollectionRoutingInfo>& routingInfo,
                                    const BSONObj shardQuery,
                                    const BSONObj collation) {
    if (mustRunOnAllShards) {
        // The pipeline begins with a stage which must be run on all shards.
        std::vector<ShardId> shardIds;
        Grid::get(opCtx)->shardRegistry()->getAllShardIds(opCtx, &shardIds);
        return {shardIds.begin(), shardIds.end()};
    }

    // If we don't need to run on all shards, then we should always have a valid routing table.
    invariant(routingInfo);

    return getTargetedShardsForQuery(opCtx, *routingInfo, shardQuery, collation);
}

std::vector<RemoteCursor> establishShardCursors(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const LiteParsedPipeline& litePipe,
    boost::optional<CachedCollectionRoutingInfo>& routingInfo,
    const BSONObj& cmdObj,
    const AggregationRequest& request,
    const ReadPreferenceSetting& readPref,
    const BSONObj& shardQuery) {
    LOG(1) << "Dispatching command " << redact(cmdObj) << " to establish cursors on shards";

    const bool mustRunOnAll = mustRunOnAllShards(nss, litePipe);
    std::set<ShardId> shardIds =
        getTargetedShards(opCtx, mustRunOnAll, routingInfo, shardQuery, request.getCollation());
    std::vector<std::pair<ShardId, BSONObj>> requests;

    // If we don't need to run on all shards, then we should always have a valid routing table.
    invariant(routingInfo || mustRunOnAll);

    if (mustRunOnAll) {
        // The pipeline contains a stage which must be run on all shards. Skip versioning and
        // enqueue the raw command objects.
        for (auto&& shardId : shardIds) {
            requests.emplace_back(std::move(shardId), cmdObj);
        }
    } else if (routingInfo->cm()) {
        // The collection is sharded. Use the routing table to decide which shards to target
        // based on the query and collation, and build versioned requests for them.
        for (auto& shardId : shardIds) {
            auto versionedCmdObj =
                appendShardVersion(cmdObj, routingInfo->cm()->getVersion(shardId));
            requests.emplace_back(std::move(shardId), std::move(versionedCmdObj));
        }
    } else {
        // The collection is unsharded. Target only the primary shard for the database.
        // Don't append shard version info when contacting the config servers.
        requests.emplace_back(routingInfo->db().primaryId(),
                              !routingInfo->db().primary()->isConfig()
                                  ? appendShardVersion(cmdObj, ChunkVersion::UNSHARDED())
                                  : cmdObj);
    }

    if (MONGO_FAIL_POINT(clusterAggregateHangBeforeEstablishingShardCursors)) {
        log() << "clusterAggregateHangBeforeEstablishingShardCursors fail point enabled.  Blocking "
                 "until fail point is disabled.";
        while (MONGO_FAIL_POINT(clusterAggregateHangBeforeEstablishingShardCursors)) {
            sleepsecs(1);
        }
    }

    return establishCursors(opCtx,
                            Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
                            nss,
                            readPref,
                            requests,
                            false /* do not allow partial results */,
                            getDesiredRetryPolicy(request));
}

std::unique_ptr<Pipeline, PipelineDeleter> targetShardsAndAddMergeCursors(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, Pipeline* ownedPipeline) {
    std::unique_ptr<Pipeline, PipelineDeleter> pipeline(ownedPipeline,
                                                        PipelineDeleter(expCtx->opCtx));

    invariant(pipeline->getSources().empty() ||
              !dynamic_cast<DocumentSourceMergeCursors*>(pipeline->getSources().front().get()));

    // Generate the command object for the targeted shards.
    std::vector<BSONObj> rawStages = [&pipeline]() {
        auto serialization = pipeline->serialize();
        std::vector<BSONObj> stages;
        stages.reserve(serialization.size());

        for (const auto& stageObj : serialization) {
            invariant(stageObj.getType() == BSONType::Object);
            stages.push_back(stageObj.getDocument().toBson());
        }

        return stages;
    }();

    AggregationRequest aggRequest(expCtx->ns, rawStages);
    LiteParsedPipeline liteParsedPipeline(aggRequest);
    auto shardDispatchResults = dispatchShardPipeline(
        expCtx, expCtx->ns, aggRequest, liteParsedPipeline, std::move(pipeline), expCtx->collation);

    std::vector<ShardId> targetedShards;
    targetedShards.reserve(shardDispatchResults.remoteCursors.size());
    for (auto&& remoteCursor : shardDispatchResults.remoteCursors) {
        targetedShards.emplace_back(remoteCursor->getShardId().toString());
    }

    std::unique_ptr<Pipeline, PipelineDeleter> mergePipeline;
    boost::optional<BSONObj> shardCursorsSortSpec = boost::none;
    if (shardDispatchResults.splitPipeline) {
        mergePipeline = std::move(shardDispatchResults.splitPipeline->mergePipeline);
        shardCursorsSortSpec = shardDispatchResults.splitPipeline->shardCursorsSortSpec;
    } else {
        // We have not split the pipeline, and will execute entirely on the remote shards. Set up an
        // empty local pipeline which we will attach the merge cursors stage to.
        mergePipeline = uassertStatusOK(Pipeline::parse(std::vector<BSONObj>(), expCtx));
    }

    cluster_aggregation_planner::addMergeCursorsSource(
        mergePipeline.get(),
        liteParsedPipeline,
        shardDispatchResults.commandForTargetedShards,
        std::move(shardDispatchResults.remoteCursors),
        targetedShards,
        shardCursorsSortSpec,
        Grid::get(expCtx->opCtx)->getExecutorPool()->getArbitraryExecutor());

    return mergePipeline;
}
}  // namespace sharded_agg_helpers
}  // namespace mongo
