/**
 * Copyright (C) 2017 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/s/commands/pipeline_s.h"

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/pipeline/document_source.h"
#include "mongo/db/query/collation/collation_spec.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/repl/read_concern_args.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/commands/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/cluster_client_cursor_impl.h"
#include "mongo/s/query/cluster_client_cursor_params.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/s/query/cluster_query_knobs.h"
#include "mongo/s/query/document_source_router_adapter.h"
#include "mongo/s/query/establish_cursors.h"
#include "mongo/s/query/establish_cursors.h"
#include "mongo/s/query/router_exec_stage.h"
#include "mongo/s/query/router_stage_internal_cursor.h"
#include "mongo/s/query/router_stage_merge.h"
#include "mongo/s/query/router_stage_update_on_add_shard.h"
#include "mongo/s/query/store_possible_cursor.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/fail_point_service.h"
#include "mongo/util/log.h"

namespace mongo {

using boost::intrusive_ptr;
using std::shared_ptr;
using std::string;
using std::unique_ptr;

MONGO_FP_DECLARE(clusterAggregateHangBeforeEstablishingShardCursors);

namespace {
/**
 * Determines the single shard to which the given query will be targeted, and its associated
 * shardVersion. Throws if the query targets more than one shard.
 */
std::pair<ShardId, ChunkVersion> getSingleTargetedShardForQuery(
    OperationContext* opCtx, const CachedCollectionRoutingInfo& routingInfo, BSONObj query) {
    if (auto chunkMgr = routingInfo.cm()) {
        std::set<ShardId> shardIds;
        chunkMgr->getShardIdsForQuery(opCtx, query, CollationSpec::kSimpleSpec, &shardIds);
        uassert(ErrorCodes::InternalError,
                str::stream() << "Unable to target lookup query to a single shard: "
                              << query.toString(),
                shardIds.size() == 1u);
        return {*shardIds.begin(), chunkMgr->getVersion(*shardIds.begin())};
    }

    return {routingInfo.primaryId(), ChunkVersion::UNSHARDED()};
}

/**
 * Returns the routing information for the namespace set on the passed ExpressionContext. Also
 * verifies that the ExpressionContext's UUID, if present, matches that of the routing table entry.
 */
StatusWith<CachedCollectionRoutingInfo> getCollectionRoutingInfo(
    const intrusive_ptr<ExpressionContext>& expCtx) {
    auto catalogCache = Grid::get(expCtx->opCtx)->catalogCache();
    auto swRoutingInfo = catalogCache->getCollectionRoutingInfo(expCtx->opCtx, expCtx->ns);
    // Additionally check that the ExpressionContext's UUID matches the collection routing info.
    if (swRoutingInfo.isOK() && expCtx->uuid && swRoutingInfo.getValue().cm()) {
        if (!swRoutingInfo.getValue().cm()->uuidMatches(*expCtx->uuid)) {
            return {ErrorCodes::NamespaceNotFound,
                    str::stream() << "The UUID of collection " << expCtx->ns.ns()
                                  << " changed; it may have been dropped and re-created."};
        }
    }
    return swRoutingInfo;
}

/**
 * Given a document representing an aggregation command such as
 *
 *  {aggregate: "myCollection", pipeline: [], ...},
 *
 * produces the corresponding explain command:
 *
 *  {explain: {aggregate: "myCollection", pipline: [], ...}, $queryOptions: {...}, verbosity: ...}
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

std::set<ShardId> getTargetedShards(OperationContext* opCtx,
                                    const NamespaceString& nss,
                                    const LiteParsedPipeline& litePipe,
                                    const CachedCollectionRoutingInfo& routingInfo,
                                    const BSONObj shardQuery,
                                    const BSONObj collation) {
    if (PipelineS::mustRunOnAllShards(nss, routingInfo, litePipe)) {
        // The pipeline begins with a stage which must be run on all shards.
        std::vector<ShardId> shardIds;
        Grid::get(opCtx)->shardRegistry()->getAllShardIds(&shardIds);
        return {shardIds.begin(), shardIds.end()};
    }

    if (routingInfo.cm()) {
        // The collection is sharded. Use the routing table to decide which shards to target
        // based on the query and collation.
        std::set<ShardId> shardIds;
        routingInfo.cm()->getShardIdsForQuery(opCtx, shardQuery, collation, &shardIds);
        return shardIds;
    }

    // The collection is unsharded. Target only the primary shard for the database.
    return {routingInfo.primaryId()};
}

StatusWith<std::vector<ClusterClientCursorParams::RemoteCursor>> establishShardCursors(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const LiteParsedPipeline& litePipe,
    CachedCollectionRoutingInfo* routingInfo,
    const BSONObj& cmdObj,
    const ReadPreferenceSetting& readPref,
    const BSONObj& shardQuery,
    const BSONObj& collation) {
    LOG(1) << "Dispatching command " << redact(cmdObj) << " to establish cursors on shards";

    std::set<ShardId> shardIds =
        getTargetedShards(opCtx, nss, litePipe, *routingInfo, shardQuery, collation);
    std::vector<std::pair<ShardId, BSONObj>> requests;

    if (PipelineS::mustRunOnAllShards(nss, *routingInfo, litePipe)) {
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
        requests.emplace_back(routingInfo->primaryId(),
                              !routingInfo->primary()->isConfig()
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

    // If we reach this point, we're either trying to establish cursors on a sharded execution
    // namespace, or handling the case where a sharded collection was dropped and recreated as
    // unsharded. Since views cannot be sharded, and because we will return an error rather than
    // attempting to continue in the event that a recreated namespace is a view, we set
    // viewDefinitionOut to nullptr.
    BSONObj* viewDefinitionOut = nullptr;
    auto swCursors = establishCursors(opCtx,
                                      Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
                                      nss,
                                      readPref,
                                      requests,
                                      false /* do not allow partial results */,
                                      viewDefinitionOut /* can't receive view definition */);

    // If any shard returned a stale shardVersion error, invalidate the routing table cache.
    // This will cause the cache to be refreshed the next time it is accessed.
    if (ErrorCodes::isStaleShardingError(swCursors.getStatus().code())) {
        Grid::get(opCtx)->catalogCache()->onStaleConfigError(std::move(*routingInfo));
    }

    return swCursors;
}

}  // namespace

bool PipelineS::mustRunOnAllShards(const NamespaceString& nss,
                                   const CachedCollectionRoutingInfo& routingInfo,
                                   const LiteParsedPipeline& litePipe) {
    // Any collectionless aggregation like a $currentOp, and a change stream on a sharded collection
    // must run on all shards.
    const bool nsIsSharded = static_cast<bool>(routingInfo.cm());
    return nss.isCollectionlessAggregateNS() || (nsIsSharded && litePipe.hasChangeStream());
}

BSONObj PipelineS::createCommandForTargetedShards(
    const AggregationRequest& request,
    BSONObj originalCmdObj,
    const std::unique_ptr<Pipeline, PipelineDeleter>& pipelineForTargetedShards) {
    // Create the command for the shards.
    MutableDocument targetedCmd(request.serializeToCommandObj());
    targetedCmd[AggregationRequest::kFromMongosName] = Value(true);

    // If 'pipelineForTargetedShards' is 'nullptr', this is an unsharded direct passthrough.
    if (pipelineForTargetedShards) {
        targetedCmd[AggregationRequest::kPipelineName] =
            Value(pipelineForTargetedShards->serialize());

        if (pipelineForTargetedShards->isSplitForShards()) {
            targetedCmd[AggregationRequest::kNeedsMergeName] = Value(true);
            targetedCmd[AggregationRequest::kCursorName] =
                Value(DOC(AggregationRequest::kBatchSizeName << 0));
        }
    }

    // If this pipeline is not split, ensure that the write concern is propagated if present.
    if (!pipelineForTargetedShards || !pipelineForTargetedShards->isSplitForShards()) {
        targetedCmd["writeConcern"] = Value(originalCmdObj["writeConcern"]);
    }

    // If this is a request for an aggregation explain, then we must wrap the aggregate inside an
    // explain command.
    if (auto explainVerbosity = request.getExplain()) {
        targetedCmd.reset(wrapAggAsExplain(targetedCmd.freeze(), *explainVerbosity));
    }

    return targetedCmd.freeze().toBson();
}

BSONObj PipelineS::establishMergingMongosCursor(
    OperationContext* opCtx,
    const AggregationRequest& request,
    const NamespaceString& requestedNss,
    BSONObj cmdToRunOnNewShards,
    const LiteParsedPipeline& liteParsedPipeline,
    std::unique_ptr<Pipeline, PipelineDeleter> pipelineForMerging,
    std::vector<ClusterClientCursorParams::RemoteCursor> cursors) {

    ClusterClientCursorParams params(
        requestedNss,
        AuthorizationSession::get(opCtx->getClient())->getAuthenticatedUserNames(),
        ReadPreferenceSetting::get(opCtx));

    params.tailableMode = pipelineForMerging->getContext()->tailableMode;
    params.mergePipeline = std::move(pipelineForMerging);
    params.remotes = std::move(cursors);

    // A batch size of 0 is legal for the initial aggregate, but not valid for getMores, the batch
    // size we pass here is used for getMores, so do not specify a batch size if the initial request
    // had a batch size of 0.
    params.batchSize = request.getBatchSize() == 0
        ? boost::none
        : boost::optional<long long>(request.getBatchSize());

    if (liteParsedPipeline.hasChangeStream()) {
        // For change streams, we need to set up a custom stage to establish cursors on new shards
        // when they are added.
        params.createCustomCursorSource = [cmdToRunOnNewShards](OperationContext* opCtx,
                                                                executor::TaskExecutor* executor,
                                                                ClusterClientCursorParams* params) {
            return stdx::make_unique<RouterStageUpdateOnAddShard>(
                opCtx, executor, params, cmdToRunOnNewShards);
        };
    }
    auto ccc = ClusterClientCursorImpl::make(
        opCtx, Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(), std::move(params));

    auto cursorState = ClusterCursorManager::CursorState::NotExhausted;
    BSONObjBuilder cursorResponse;

    CursorResponseBuilder responseBuilder(true, &cursorResponse);

    for (long long objCount = 0; objCount < request.getBatchSize(); ++objCount) {
        ClusterQueryResult next;
        try {
            next = uassertStatusOK(ccc->next(RouterExecStage::ExecContext::kInitialFind));
        } catch (const ExceptionFor<ErrorCodes::CloseChangeStream>&) {
            // This exception is thrown when a $changeStream stage encounters an event
            // that invalidates the cursor. We should close the cursor and return without
            // error.
            cursorState = ClusterCursorManager::CursorState::Exhausted;
            break;
        }

        // Check whether we have exhausted the pipeline's results.
        if (next.isEOF()) {
            // We reached end-of-stream. If the cursor is not tailable, then we mark it as
            // exhausted. If it is tailable, usually we keep it open (i.e. "NotExhausted") even when
            // we reach end-of-stream. However, if all the remote cursors are exhausted, there is no
            // hope of returning data and thus we need to close the mongos cursor as well.
            if (!ccc->isTailable() || ccc->remotesExhausted()) {
                cursorState = ClusterCursorManager::CursorState::Exhausted;
            }
            break;
        }

        // If this result will fit into the current batch, add it. Otherwise, stash it in the cursor
        // to be returned on the next getMore.
        auto nextObj = *next.getResult();

        if (!FindCommon::haveSpaceForNext(nextObj, objCount, responseBuilder.bytesUsed())) {
            ccc->queueResult(nextObj);
            break;
        }

        responseBuilder.append(nextObj);
    }

    ccc->detachFromOperationContext();

    CursorId clusterCursorId = 0;

    if (cursorState == ClusterCursorManager::CursorState::NotExhausted) {
        clusterCursorId = uassertStatusOK(Grid::get(opCtx)->getCursorManager()->registerCursor(
            opCtx,
            ccc.releaseCursor(),
            requestedNss,
            ClusterCursorManager::CursorType::MultiTarget,
            ClusterCursorManager::CursorLifetime::Mortal));
    }

    responseBuilder.done(clusterCursorId, requestedNss.ns());

    Command::appendCommandStatus(cursorResponse, Status::OK());

    return cursorResponse.obj();
}

/**
 * Targets shards for the pipeline and returns a struct with the remote cursors or results, and
 * the pipeline that will need to be executed to merge the results from the remotes. If a stale
 * shard version is encountered, refreshes the routing table and tries again.
 */
StatusWith<PipelineS::DispatchShardPipelineResults> PipelineS::dispatchShardPipeline(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& executionNss,
    BSONObj originalCmdObj,
    const AggregationRequest& aggRequest,
    const LiteParsedPipeline& liteParsedPipeline,
    std::unique_ptr<Pipeline, PipelineDeleter> pipeline) {
    // The process is as follows:
    // - First, determine whether we need to target more than one shard. If so, we split the
    // pipeline; if not, we retain the existing pipeline.
    // - Call establishShardCursors to dispatch the aggregation to the targeted shards.
    // - If we get a staleConfig exception, re-evaluate whether we need to split the pipeline with
    // the refreshed routing table data.
    // - If the pipeline is not split and we now need to target multiple shards, split it. If the
    // pipeline is already split and we now only need to target a single shard, reassemble the
    // original pipeline.
    // - After exhausting 10 attempts to establish the cursors, we give up and throw.
    auto swCursors = makeStatusWith<std::vector<ClusterClientCursorParams::RemoteCursor>>();
    auto swShardResults = makeStatusWith<std::vector<AsyncRequestsSender::Response>>();
    auto opCtx = expCtx->opCtx;

    const bool needsPrimaryShardMerge =
        (pipeline->needsPrimaryShardMerger() || internalQueryAlwaysMergeOnPrimaryShard.load());

    const bool needsMongosMerge = pipeline->needsMongosMerger();

    const auto shardQuery = pipeline->getInitialQuery();

    auto pipelineForTargetedShards = std::move(pipeline);
    std::unique_ptr<Pipeline, PipelineDeleter> pipelineForMerging;
    BSONObj targetedCommand;

    int numAttempts = 0;

    do {
        // We need to grab a new routing table at the start of each iteration, since a stale config
        // exception will invalidate the previous one.
        auto executionNsRoutingInfo = uassertStatusOK(
            Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, executionNss));

        // Determine whether we can run the entire aggregation on a single shard.
        std::set<ShardId> shardIds = getTargetedShards(opCtx,
                                                       executionNss,
                                                       liteParsedPipeline,
                                                       executionNsRoutingInfo,
                                                       shardQuery,
                                                       aggRequest.getCollation());

        uassert(ErrorCodes::ShardNotFound,
                "No targets were found for this aggregation. All shards were removed from the "
                "cluster mid-operation",
                shardIds.size() > 0);

        // Don't need to split the pipeline if we are only targeting a single shard, unless:
        // - There is a stage that needs to be run on the primary shard and the single target shard
        //   is not the primary.
        // - The pipeline contains one or more stages which must always merge on mongoS.
        const bool needsSplit =
            (shardIds.size() > 1u || needsMongosMerge ||
             (needsPrimaryShardMerge && *(shardIds.begin()) != executionNsRoutingInfo.primaryId()));

        const bool isSplit = pipelineForTargetedShards->isSplitForShards();

        // If we have to run on multiple shards and the pipeline is not yet split, split it. If we
        // can run on a single shard and the pipeline is already split, reassemble it.
        if (needsSplit && !isSplit) {
            pipelineForMerging = std::move(pipelineForTargetedShards);
            pipelineForTargetedShards = pipelineForMerging->splitForSharded();
        } else if (!needsSplit && isSplit) {
            pipelineForTargetedShards->unsplitFromSharded(std::move(pipelineForMerging));
        }

        // Generate the command object for the targeted shards.
        targetedCommand = PipelineS::createCommandForTargetedShards(
            aggRequest, originalCmdObj, pipelineForTargetedShards);

        // Refresh the shard registry if we're targeting all shards.  We need the shard registry
        // to be at least as current as the logical time used when creating the command for
        // $changeStream to work reliably, so we do a "hard" reload.
        if (mustRunOnAllShards(executionNss, executionNsRoutingInfo, liteParsedPipeline)) {
            auto* shardRegistry = Grid::get(opCtx)->shardRegistry();
            if (!shardRegistry->reload(opCtx)) {
                shardRegistry->reload(opCtx);
            }
        }

        // Explain does not produce a cursor, so instead we scatter-gather commands to the shards.
        if (expCtx->explain) {
            if (mustRunOnAllShards(executionNss, executionNsRoutingInfo, liteParsedPipeline)) {
                // Some stages (such as $currentOp) need to be broadcast to all shards, and should
                // not participate in the shard version protocol.
                swShardResults =
                    scatterGatherUnversionedTargetAllShards(opCtx,
                                                            executionNss.db().toString(),
                                                            executionNss,
                                                            targetedCommand,
                                                            ReadPreferenceSetting::get(opCtx),
                                                            Shard::RetryPolicy::kIdempotent);
            } else {
                // Aggregations on a real namespace should use the routing table to target shards,
                // and should participate in the shard version protocol.
                swShardResults =
                    scatterGatherVersionedTargetByRoutingTable(opCtx,
                                                               executionNss.db().toString(),
                                                               executionNss,
                                                               targetedCommand,
                                                               ReadPreferenceSetting::get(opCtx),
                                                               Shard::RetryPolicy::kIdempotent,
                                                               shardQuery,
                                                               aggRequest.getCollation(),
                                                               nullptr /* viewDefinition */);
            }
        } else {
            swCursors = establishShardCursors(opCtx,
                                              executionNss,
                                              liteParsedPipeline,
                                              &executionNsRoutingInfo,
                                              targetedCommand,
                                              ReadPreferenceSetting::get(opCtx),
                                              shardQuery,
                                              aggRequest.getCollation());

            if (ErrorCodes::isStaleShardingError(swCursors.getStatus().code())) {
                LOG(1) << "got stale shardVersion error " << swCursors.getStatus()
                       << " while dispatching " << redact(targetedCommand) << " after "
                       << (numAttempts + 1) << " dispatch attempts";
            }
        }
    } while (++numAttempts < kMaxNumStaleVersionRetries &&
             (expCtx->explain ? !swShardResults.isOK() : !swCursors.isOK()));

    if (!swShardResults.isOK()) {
        return swShardResults.getStatus();
    }
    if (!swCursors.isOK()) {
        return swCursors.getStatus();
    }
    return DispatchShardPipelineResults{needsPrimaryShardMerge,
                                        std::move(swCursors.getValue()),
                                        std::move(swShardResults.getValue()),
                                        std::move(pipelineForTargetedShards),
                                        std::move(pipelineForMerging),
                                        targetedCommand};
}

StatusWith<std::unique_ptr<Pipeline, PipelineDeleter>> PipelineS::MongoSInterface::makePipeline(
    const std::vector<BSONObj>& rawPipeline,
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const MakePipelineOptions pipelineOptions) {
    // Explain is not supported for auxiliary lookups.
    invariant(!expCtx->explain);

    // Temporarily remove any deadline from this operation, we don't want to timeout while doing
    // this lookup.
    OperationContext::DeadlineStash deadlineStash(expCtx->opCtx);

    auto pipeline = Pipeline::parse(rawPipeline, expCtx);
    if (!pipeline.isOK()) {
        return pipeline.getStatus();
    }
    if (pipelineOptions.optimize) {
        pipeline.getValue()->optimizePipeline();
    }
    if (pipelineOptions.attachCursorSource) {
        pipeline = attachCursorSourceToPipeline(expCtx, pipeline.getValue().release());
    }

    return pipeline;
}

StatusWith<unique_ptr<Pipeline, PipelineDeleter>>
PipelineS::MongoSInterface::attachCursorSourceToPipeline(
    const boost::intrusive_ptr<ExpressionContext>& expCtx, Pipeline* pipeline) {
    invariant(pipeline->getSources().empty() ||
              !dynamic_cast<DocumentSourceRouterAdapter*>(pipeline->getSources().front().get()));

    // Generate the command object for the targeted shards.
    auto serialization = pipeline->serialize();
    std::vector<BSONObj> rawStages;
    rawStages.reserve(serialization.size());
    std::transform(serialization.begin(),
                   serialization.end(),
                   std::back_inserter(rawStages),
                   [](const Value& stageObj) {
                       invariant(stageObj.getType() == BSONType::Object);
                       return stageObj.getDocument().toBson();
                   });
    AggregationRequest aggRequest(expCtx->ns, rawStages);
    LiteParsedPipeline liteParsedPipeline(aggRequest);
    auto dispatchStatus = PipelineS::dispatchShardPipeline(
        expCtx,
        expCtx->ns,
        aggRequest.serializeToCommandObj().toBson(),
        aggRequest,
        liteParsedPipeline,
        std::unique_ptr<Pipeline, PipelineDeleter>(pipeline, PipelineDeleter(expCtx->opCtx)));

    if (!dispatchStatus.isOK()) {
        return dispatchStatus.getStatus();
    }
    auto targetingResults = std::move(dispatchStatus.getValue());

    auto params = stdx::make_unique<ClusterClientCursorParams>(
        expCtx->ns,
        AuthorizationSession::get(expCtx->opCtx->getClient())->getAuthenticatedUserNames(),
        ReadPreferenceSetting::get(expCtx->opCtx));
    params->remotes = std::move(targetingResults.remoteCursors);
    params->mergePipeline = std::move(targetingResults.pipelineForMerging);

    // We will transfer ownership of the params to the RouterStageInternalCursor, but need a
    // reference to them to construct the RouterStageMerge.
    auto* unownedParams = params.get();
    auto root = ClusterClientCursorImpl::buildMergerPlan(
        expCtx->opCtx,
        Grid::get(expCtx->opCtx)->getExecutorPool()->getArbitraryExecutor(),
        unownedParams);
    auto routerExecutionTree = stdx::make_unique<RouterStageInternalCursor>(
        expCtx->opCtx, std::move(params), std::move(root));

    return Pipeline::create(
        {DocumentSourceRouterAdapter::create(expCtx, std::move(routerExecutionTree))}, expCtx);
}

boost::optional<Document> PipelineS::MongoSInterface::lookupSingleDocument(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& nss,
    UUID collectionUUID,
    const Document& filter,
    boost::optional<BSONObj> readConcern) {
    auto foreignExpCtx = expCtx->copyWith(nss, collectionUUID);

    // Create the find command to be dispatched to the shard in order to return the post-change
    // document.
    auto filterObj = filter.toBson();
    BSONObjBuilder cmdBuilder;
    bool findCmdIsByUuid(foreignExpCtx->uuid);
    if (findCmdIsByUuid) {
        foreignExpCtx->uuid->appendToBuilder(&cmdBuilder, "find");
    } else {
        cmdBuilder.append("find", nss.coll());
    }
    cmdBuilder.append("filter", filterObj);
    cmdBuilder.append("comment", expCtx->comment);
    if (readConcern) {
        cmdBuilder.append(repl::ReadConcernArgs::kReadConcernFieldName, *readConcern);
    }

    auto swShardResult = makeStatusWith<std::vector<ClusterClientCursorParams::RemoteCursor>>();
    auto findCmd = cmdBuilder.obj();
    size_t numAttempts = 0;
    do {
        // Verify that the collection exists, with the correct UUID.
        auto catalogCache = Grid::get(expCtx->opCtx)->catalogCache();
        auto swRoutingInfo = getCollectionRoutingInfo(foreignExpCtx);
        if (swRoutingInfo == ErrorCodes::NamespaceNotFound) {
            return boost::none;
        }
        auto routingInfo = uassertStatusOK(std::move(swRoutingInfo));
        if (findCmdIsByUuid && routingInfo.cm()) {
            // Find by UUID and shard versioning do not work together (SERVER-31946).  In the
            // sharded case we've already checked the UUID, so find by namespace is safe.  In the
            // unlikely case that the collection has been deleted and a new collection with the same
            // name created through a different mongos, the shard version will be detected as stale,
            // as shard versions contain an 'epoch' field unique to the collection.
            findCmd = findCmd.addField(BSON("find" << nss.coll()).firstElement());
            findCmdIsByUuid = false;
        }

        // Get the ID and version of the single shard to which this query will be sent.
        auto shardInfo = getSingleTargetedShardForQuery(expCtx->opCtx, routingInfo, filterObj);

        // Dispatch the request. This will only be sent to a single shard and only a single result
        // will be returned. The 'establishCursors' method conveniently prepares the result into a
        // cursor response for us.
        swShardResult =
            establishCursors(expCtx->opCtx,
                             Grid::get(expCtx->opCtx)->getExecutorPool()->getArbitraryExecutor(),
                             nss,
                             ReadPreferenceSetting::get(expCtx->opCtx),
                             {{shardInfo.first, appendShardVersion(findCmd, shardInfo.second)}},
                             false,
                             nullptr);

        // If it's an unsharded collection which has been deleted and re-created, we may get a
        // NamespaceNotFound error when looking up by UUID.
        if (swShardResult.getStatus().code() == ErrorCodes::NamespaceNotFound) {
            return boost::none;
        }
        // If we hit a stale shardVersion exception, invalidate the routing table cache.
        if (ErrorCodes::isStaleShardingError(swShardResult.getStatus().code())) {
            catalogCache->onStaleConfigError(std::move(routingInfo));
        }
    } while (!swShardResult.isOK() && ++numAttempts < kMaxNumStaleVersionRetries);

    auto shardResult = uassertStatusOK(std::move(swShardResult));
    invariant(shardResult.size() == 1u);

    auto& cursor = shardResult.front().cursorResponse;
    auto& batch = cursor.getBatch();

    // We should have at most 1 result, and the cursor should be exhausted.
    uassert(ErrorCodes::InternalError,
            str::stream() << "Shard cursor was unexpectedly open after lookup: "
                          << shardResult.front().hostAndPort
                          << ", id: "
                          << cursor.getCursorId(),
            cursor.getCursorId() == 0);
    uassert(ErrorCodes::TooManyMatchingDocuments,
            str::stream() << "found more than one document matching " << filter.toString() << " ["
                          << batch.begin()->toString()
                          << ", "
                          << std::next(batch.begin())->toString()
                          << "]",
            batch.size() <= 1u);

    return (!batch.empty() ? Document(batch.front()) : boost::optional<Document>{});
}

std::vector<GenericCursor> PipelineS::MongoSInterface::getCursors(
    const intrusive_ptr<ExpressionContext>& expCtx) const {
    invariant(hasGlobalServiceContext());
    auto cursorManager = Grid::get(expCtx->opCtx->getServiceContext())->getCursorManager();
    invariant(cursorManager);
    return cursorManager->getAllCursors();
}

bool PipelineS::MongoSInterface::isSharded(OperationContext* opCtx, const NamespaceString& nss) {
    const auto catalogCache = Grid::get(opCtx)->catalogCache();

    auto routingInfoStatus = catalogCache->getCollectionRoutingInfo(opCtx, nss);

    if (!routingInfoStatus.isOK()) {
        // db doesn't exist.
        return false;
    }
    return static_cast<bool>(routingInfoStatus.getValue().cm());
}

}  // namespace mongo
