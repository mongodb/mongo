/**
*    Copyright (C) 2016 MongoDB Inc.
*
*    This program is free software: you can redistribute it and/or  modify
*    it under the terms of the GNU Affero General Public License, version 3,
*    as published by the Free Software Foundation.
*
*    This program is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU Affero General Public License for more details.
*
*    You should have received a copy of the GNU Affero General Public License
*    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*    As a special exception, the copyright holders give permission to link the
*    code of portions of this program with the OpenSSL library under certain
*    conditions as described in each individual source file and distribute
*    linked combinations including the program with the OpenSSL library. You
*    must comply with the GNU Affero General Public License in all respects for
*    all of the code used other than as permitted herein. If you modify file(s)
*    with this exception, you may extend this exception to your version of the
*    file(s), but you are not obligated to do so. If you do not wish to do so,
*    delete this exception statement from your version. If you delete this
*    exception statement from all source files in the program, then also delete
*    it in the license file.
*/

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kCommand

#include "mongo/platform/basic.h"

#include "mongo/s/commands/cluster_aggregate.h"

#include <boost/intrusive_ptr.hpp>

#include "mongo/bson/util/bson_extract.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source_change_stream.h"
#include "mongo/db/pipeline/document_source_out.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/query/cursor_response.h"
#include "mongo/db/query/find_common.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/db/views/view.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_connection.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/commands/cluster_commands_helpers.h"
#include "mongo/s/commands/pipeline_s.h"
#include "mongo/s/grid.h"
#include "mongo/s/query/cluster_client_cursor_impl.h"
#include "mongo/s/query/cluster_client_cursor_params.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/s/query/cluster_query_knobs.h"
#include "mongo/s/query/establish_cursors.h"
#include "mongo/s/query/router_stage_update_on_add_shard.h"
#include "mongo/s/query/store_possible_cursor.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/log.h"

namespace mongo {

MONGO_FP_DECLARE(clusterAggregateHangBeforeEstablishingShardCursors);

namespace {
// Given a document representing an aggregation command such as
//
//   {aggregate: "myCollection", pipeline: [], ...},
//
// produces the corresponding explain command:
//
//   {explain: {aggregate: "myCollection", pipline: [], ...}, $queryOptions: {...}, verbosity: ...}
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

Status appendExplainResults(
    const std::vector<AsyncRequestsSender::Response>& shardResults,
    const boost::intrusive_ptr<ExpressionContext>& mergeCtx,
    const std::unique_ptr<Pipeline, Pipeline::Deleter>& pipelineForTargetedShards,
    const std::unique_ptr<Pipeline, Pipeline::Deleter>& pipelineForMerging,
    BSONObjBuilder* result) {
    if (pipelineForTargetedShards->isSplitForShards()) {
        *result << "mergeType"
                << (pipelineForMerging->canRunOnMongos()
                        ? "mongos"
                        : pipelineForMerging->needsPrimaryShardMerger() ? "primaryShard"
                                                                        : "anyShard")
                << "splitPipeline"
                << Document{
                       {"shardsPart",
                        pipelineForTargetedShards->writeExplainOps(*mergeCtx->explain)},
                       {"mergerPart", pipelineForMerging->writeExplainOps(*mergeCtx->explain)}};
    } else {
        *result << "splitPipeline" << BSONNULL;
    }

    BSONObjBuilder shardExplains(result->subobjStart("shards"));
    for (const auto& shardResult : shardResults) {
        invariant(shardResult.shardHostAndPort);
        shardExplains.append(shardResult.shardId.toString(),
                             BSON("host" << shardResult.shardHostAndPort->toString() << "stages"
                                         << shardResult.swResponse.getValue().data["stages"]));
    }

    return Status::OK();
}

Status appendCursorResponseToCommandResult(const ShardId& shardId,
                                           const BSONObj cursorResponse,
                                           BSONObjBuilder* result) {
    // If a write error was encountered, append it to the output buffer first.
    if (auto wcErrorElem = cursorResponse["writeConcernError"]) {
        appendWriteConcernErrorToCmdResponse(shardId, wcErrorElem, *result);
    }

    // Pass the results from the remote shard into our command response.
    result->appendElementsUnique(Command::filterCommandReplyForPassthrough(cursorResponse));
    return getStatusFromCommandResult(result->asTempObj());
}

bool mustRunOnAllShards(const NamespaceString& nss,
                        const CachedCollectionRoutingInfo& routingInfo,
                        const LiteParsedPipeline& litePipe) {
    // Any collectionless aggregation like a $currentOp, and a change stream on a sharded collection
    // must run on all shards.
    const bool nsIsSharded = static_cast<bool>(routingInfo.cm());
    return nss.isCollectionlessAggregateNS() || (nsIsSharded && litePipe.hasChangeStream());
}

StatusWith<CachedCollectionRoutingInfo> getExecutionNsRoutingInfo(OperationContext* opCtx,
                                                                  const NamespaceString& execNss,
                                                                  CatalogCache* catalogCache) {
    // This call to getCollectionRoutingInfo will return !OK if the database does not exist.
    auto swRoutingInfo = catalogCache->getCollectionRoutingInfo(opCtx, execNss);

    // Collectionless aggregations, however, may be run on 'admin' (which should always exist) but
    // are subsequently targeted towards the shards. If getCollectionRoutingInfo is OK, we perform a
    // further check that at least one shard exists if the aggregation is collectionless.
    if (swRoutingInfo.isOK() && execNss.isCollectionlessAggregateNS()) {
        std::vector<ShardId> shardIds;
        Grid::get(opCtx)->shardRegistry()->getAllShardIds(&shardIds);

        if (shardIds.size() == 0) {
            return {ErrorCodes::NamespaceNotFound, "No shards are present in the cluster"};
        }
    }

    return swRoutingInfo;
}

std::set<ShardId> getTargetedShards(OperationContext* opCtx,
                                    const NamespaceString& nss,
                                    const LiteParsedPipeline& litePipe,
                                    const CachedCollectionRoutingInfo& routingInfo,
                                    const BSONObj shardQuery,
                                    const BSONObj collation) {
    if (mustRunOnAllShards(nss, routingInfo, litePipe)) {
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

BSONObj createCommandForTargetedShards(
    const AggregationRequest& request,
    const BSONObj originalCmdObj,
    const std::unique_ptr<Pipeline, Pipeline::Deleter>& pipelineForTargetedShards) {
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

BSONObj createCommandForMergingShard(
    const AggregationRequest& request,
    const boost::intrusive_ptr<ExpressionContext>& mergeCtx,
    const BSONObj originalCmdObj,
    const std::unique_ptr<Pipeline, Pipeline::Deleter>& pipelineForMerging) {
    MutableDocument mergeCmd(request.serializeToCommandObj());

    mergeCmd["pipeline"] = Value(pipelineForMerging->serialize());
    mergeCmd[AggregationRequest::kFromMongosName] = Value(true);
    mergeCmd["writeConcern"] = Value(originalCmdObj["writeConcern"]);

    // If the user didn't specify a collation already, make sure there's a collation attached to
    // the merge command, since the merging shard may not have the collection metadata.
    if (mergeCmd.peek()["collation"].missing()) {
        mergeCmd["collation"] = mergeCtx->getCollator()
            ? Value(mergeCtx->getCollator()->getSpec().toBSON())
            : Value(Document{CollationSpec::kSimpleSpec});
    }

    return mergeCmd.freeze().toBson();
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

    if (mustRunOnAllShards(nss, *routingInfo, litePipe)) {
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

struct DispatchShardPipelineResults {
    // True if this pipeline was split, and the second half of the pipeline needs to be run on the
    // primary shard for the database.
    bool needsPrimaryShardMerge;

    // Populated if this *is not* an explain, this vector represents the cursors on the remote
    // shards.
    std::vector<ClusterClientCursorParams::RemoteCursor> remoteCursors;

    // Populated if this *is* an explain, this vector represents the results from each shard.
    std::vector<AsyncRequestsSender::Response> remoteExplainOutput;

    // The half of the pipeline that was sent to each shard, or the entire pipeline if there was
    // only one shard targeted.
    std::unique_ptr<Pipeline, Pipeline::Deleter> pipelineForTargetedShards;

    // The merging half of the pipeline if more than one shard was targeted, otherwise nullptr.
    std::unique_ptr<Pipeline, Pipeline::Deleter> pipelineForMerging;

    // The command object to send to the targeted shards.
    BSONObj commandForTargetedShards;
};

/**
 * Targets shards for the pipeline and returns a struct with the remote cursors or results, and
 * the pipeline that will need to be executed to merge the results from the remotes. If a stale
 * shard version is encountered, refreshes the routing table and tries again.
 */
StatusWith<DispatchShardPipelineResults> dispatchShardPipeline(
    const boost::intrusive_ptr<ExpressionContext>& expCtx,
    const NamespaceString& executionNss,
    BSONObj originalCmdObj,
    const AggregationRequest& aggRequest,
    const LiteParsedPipeline& liteParsedPipeline,
    std::unique_ptr<Pipeline, Pipeline::Deleter> pipeline) {
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
    std::unique_ptr<Pipeline, Pipeline::Deleter> pipelineForMerging;
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
        targetedCommand =
            createCommandForTargetedShards(aggRequest, originalCmdObj, pipelineForTargetedShards);

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

StatusWith<std::pair<ShardId, Shard::CommandResponse>> establishMergingShardCursor(
    OperationContext* opCtx,
    const NamespaceString& nss,
    const std::vector<ClusterClientCursorParams::RemoteCursor>& cursors,
    const BSONObj mergeCmdObj,
    const boost::optional<ShardId> primaryShard) {
    // Run merging command on random shard, unless we need to run on the primary shard.
    auto& prng = opCtx->getClient()->getPrng();
    const auto mergingShardId =
        primaryShard ? primaryShard.get() : cursors[prng.nextInt32(cursors.size())].shardId;
    const auto mergingShard =
        uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, mergingShardId));

    auto shardCmdResponse = uassertStatusOK(
        mergingShard->runCommandWithFixedRetryAttempts(opCtx,
                                                       ReadPreferenceSetting::get(opCtx),
                                                       nss.db().toString(),
                                                       mergeCmdObj,
                                                       Shard::RetryPolicy::kIdempotent));

    return {{std::move(mergingShardId), std::move(shardCmdResponse)}};
}

BSONObj establishMergingMongosCursor(
    OperationContext* opCtx,
    const AggregationRequest& request,
    const NamespaceString& requestedNss,
    BSONObj cmdToRunOnNewShards,
    const LiteParsedPipeline& liteParsedPipeline,
    std::unique_ptr<Pipeline, Pipeline::Deleter> pipelineForMerging,
    std::vector<ClusterClientCursorParams::RemoteCursor> cursors) {

    // Inject the MongosProcessInterface for sources which need it.
    PipelineS::injectMongosInterface(pipelineForMerging.get());

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

BSONObj getDefaultCollationForUnshardedCollection(const Shard* primaryShard,
                                                  const NamespaceString& nss) {
    ScopedDbConnection conn(primaryShard->getConnString());
    BSONObj defaultCollation;
    std::list<BSONObj> all =
        conn->getCollectionInfos(nss.db().toString(), BSON("name" << nss.coll()));
    if (all.empty()) {
        return defaultCollation;
    }
    BSONObj collectionInfo = all.front();
    if (collectionInfo["options"].type() == BSONType::Object) {
        BSONObj collectionOptions = collectionInfo["options"].Obj();
        BSONElement collationElement;
        auto status = bsonExtractTypedField(
            collectionOptions, "collation", BSONType::Object, &collationElement);
        if (status.isOK()) {
            defaultCollation = collationElement.Obj().getOwned();
            uassert(ErrorCodes::BadValue,
                    "Default collation in collection metadata cannot be empty.",
                    !defaultCollation.isEmpty());
        } else if (status != ErrorCodes::NoSuchKey) {
            uassertStatusOK(status);
        }
    }
    return defaultCollation;
}

}  // namespace

Status ClusterAggregate::runAggregate(OperationContext* opCtx,
                                      const Namespaces& namespaces,
                                      const AggregationRequest& request,
                                      BSONObj cmdObj,
                                      BSONObjBuilder* result) {
    const auto catalogCache = Grid::get(opCtx)->catalogCache();

    auto executionNsRoutingInfoStatus =
        getExecutionNsRoutingInfo(opCtx, namespaces.executionNss, catalogCache);

    LiteParsedPipeline liteParsedPipeline(request);

    if (!executionNsRoutingInfoStatus.isOK()) {
        // Standard aggregations swallow 'NamespaceNotFound' and return an empty cursor with id 0 in
        // the event that the database does not exist. For $changeStream aggregations, however, we
        // throw the exception in all error cases, including that of a non-existent database.
        uassert(executionNsRoutingInfoStatus.getStatus().code(),
                str::stream() << "failed to open $changeStream: "
                              << executionNsRoutingInfoStatus.getStatus().reason(),
                !liteParsedPipeline.hasChangeStream());
        appendEmptyResultSet(
            *result, executionNsRoutingInfoStatus.getStatus(), namespaces.requestedNss.ns());
        return Status::OK();
    }

    auto executionNsRoutingInfo = executionNsRoutingInfoStatus.getValue();

    // Determine the appropriate collation and 'resolve' involved namespaces to make the
    // ExpressionContext.

    // We won't try to execute anything on a mongos, but we still have to populate this map so that
    // any $lookups, etc. will be able to have a resolved view definition. It's okay that this is
    // incorrect, we will repopulate the real resolved namespace map on the mongod. Note that we
    // need to check if any involved collections are sharded before forwarding an aggregation
    // command on an unsharded collection.
    StringMap<ExpressionContext::ResolvedNamespace> resolvedNamespaces;

    for (auto&& nss : liteParsedPipeline.getInvolvedNamespaces()) {
        const auto resolvedNsRoutingInfo =
            uassertStatusOK(catalogCache->getCollectionRoutingInfo(opCtx, nss));
        uassert(
            28769, str::stream() << nss.ns() << " cannot be sharded", !resolvedNsRoutingInfo.cm());
        resolvedNamespaces.try_emplace(nss.coll(), nss, std::vector<BSONObj>{});
    }

    // If this pipeline is on an unsharded collection, is allowed to be forwarded to shards, does
    // not need to run on all shards, and doesn't need transformation via
    // DocumentSource::serialize(), then go ahead and pass it through to the owning shard
    // unmodified.
    if (!executionNsRoutingInfo.cm() &&
        !mustRunOnAllShards(namespaces.executionNss, executionNsRoutingInfo, liteParsedPipeline) &&
        liteParsedPipeline.allowedToForwardFromMongos() &&
        liteParsedPipeline.allowedToPassthroughFromMongos()) {
        return aggPassthrough(opCtx,
                              namespaces,
                              executionNsRoutingInfo.primary()->getId(),
                              cmdObj,
                              request,
                              liteParsedPipeline,
                              result);
    }

    std::unique_ptr<CollatorInterface> collation;
    if (!request.getCollation().isEmpty()) {
        collation = uassertStatusOK(CollatorFactoryInterface::get(opCtx->getServiceContext())
                                        ->makeFromBSON(request.getCollation()));
    } else if (const auto chunkMgr = executionNsRoutingInfo.cm()) {
        if (chunkMgr->getDefaultCollator()) {
            collation = chunkMgr->getDefaultCollator()->clone();
        }
    } else {
        // Unsharded collection.  Get collection metadata from primary chunk.
        auto collationObj = getDefaultCollationForUnshardedCollection(
            executionNsRoutingInfo.primary().get(), namespaces.executionNss);
        if (!collationObj.isEmpty()) {
            collation = uassertStatusOK(CollatorFactoryInterface::get(opCtx->getServiceContext())
                                            ->makeFromBSON(collationObj));
        }
    }

    boost::intrusive_ptr<ExpressionContext> mergeCtx =
        new ExpressionContext(opCtx, request, std::move(collation), std::move(resolvedNamespaces));
    mergeCtx->inMongos = true;
    // explicitly *not* setting mergeCtx->tempDir

    auto pipeline = uassertStatusOK(Pipeline::parse(request.getPipeline(), mergeCtx));
    pipeline->optimizePipeline();

    // Check whether the entire pipeline must be run on mongoS.
    if (pipeline->requiredToRunOnMongos()) {
        uassert(ErrorCodes::IllegalOperation,
                str::stream() << "Aggregation pipeline must be run on mongoS, but "
                              << pipeline->getSources().front()->getSourceName()
                              << " is not capable of producing input",
                !pipeline->getSources().front()->constraints().requiresInputDocSource);

        auto cursorResponse = establishMergingMongosCursor(opCtx,
                                                           request,
                                                           namespaces.requestedNss,
                                                           cmdObj,
                                                           liteParsedPipeline,
                                                           std::move(pipeline),
                                                           {});
        Command::filterCommandReplyForPassthrough(cursorResponse, result);
        return getStatusFromCommandResult(result->asTempObj());
    }

    auto dispatchResults = uassertStatusOK(dispatchShardPipeline(mergeCtx,
                                                                 namespaces.executionNss,
                                                                 cmdObj,
                                                                 request,
                                                                 liteParsedPipeline,
                                                                 std::move(pipeline)));

    if (mergeCtx->explain) {
        // If we reach here, we've either succeeded in running the explain or exhausted all
        // attempts. In either case, attempt to append the explain results to the output builder.
        uassertAllShardsSupportExplain(dispatchResults.remoteExplainOutput);

        return appendExplainResults(std::move(dispatchResults.remoteExplainOutput),
                                    mergeCtx,
                                    dispatchResults.pipelineForTargetedShards,
                                    dispatchResults.pipelineForMerging,
                                    result);
    }


    invariant(dispatchResults.remoteCursors.size() > 0);

    // If we dispatched to a single shard, store the remote cursor and return immediately.
    if (!dispatchResults.pipelineForTargetedShards->isSplitForShards()) {
        invariant(dispatchResults.remoteCursors.size() == 1);
        const auto& remoteCursor = dispatchResults.remoteCursors[0];
        auto executorPool = Grid::get(opCtx)->getExecutorPool();
        const BSONObj reply = uassertStatusOK(storePossibleCursor(
            opCtx,
            remoteCursor.shardId,
            remoteCursor.hostAndPort,
            remoteCursor.cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse),
            namespaces.requestedNss,
            executorPool->getArbitraryExecutor(),
            Grid::get(opCtx)->getCursorManager(),
            mergeCtx->tailableMode));

        return appendCursorResponseToCommandResult(remoteCursor.shardId, reply, result);
    }

    // If we reach here, we have a merge pipeline to dispatch.
    auto mergingPipeline = std::move(dispatchResults.pipelineForMerging);
    invariant(mergingPipeline);

    // First, check whether we can merge on the mongoS. If the merge pipeline MUST run on mongoS,
    // then ignore the internalQueryProhibitMergingOnMongoS parameter.
    if (mergingPipeline->requiredToRunOnMongos() ||
        (!internalQueryProhibitMergingOnMongoS.load() && mergingPipeline->canRunOnMongos())) {
        // Register the new mongoS cursor, and retrieve the initial batch of results.
        auto cursorResponse =
            establishMergingMongosCursor(opCtx,
                                         request,
                                         namespaces.requestedNss,
                                         dispatchResults.commandForTargetedShards,
                                         liteParsedPipeline,
                                         std::move(mergingPipeline),
                                         std::move(dispatchResults.remoteCursors));

        // We don't need to storePossibleCursor or propagate writeConcern errors; an $out pipeline
        // can never run on mongoS. Filter the command response and return immediately.
        Command::filterCommandReplyForPassthrough(cursorResponse, result);
        return getStatusFromCommandResult(result->asTempObj());
    }

    // If we cannot merge on mongoS, establish the merge cursor on a shard.
    mergingPipeline->addInitialSource(
        DocumentSourceMergeCursors::create(parseCursors(dispatchResults.remoteCursors), mergeCtx));
    auto mergeCmdObj = createCommandForMergingShard(request, mergeCtx, cmdObj, mergingPipeline);

    auto mergeResponse = uassertStatusOK(
        establishMergingShardCursor(opCtx,
                                    namespaces.executionNss,
                                    dispatchResults.remoteCursors,
                                    mergeCmdObj,
                                    boost::optional<ShardId>{dispatchResults.needsPrimaryShardMerge,
                                                             executionNsRoutingInfo.primaryId()}));

    auto mergingShardId = mergeResponse.first;
    auto response = mergeResponse.second;

    // The merging shard is remote, so if a response was received, a HostAndPort must have been set.
    invariant(response.hostAndPort);
    auto mergeCursorResponse = uassertStatusOK(
        storePossibleCursor(opCtx,
                            mergingShardId,
                            *response.hostAndPort,
                            response.response,
                            namespaces.requestedNss,
                            Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
                            Grid::get(opCtx)->getCursorManager()));

    return appendCursorResponseToCommandResult(mergingShardId, mergeCursorResponse, result);
}

std::vector<DocumentSourceMergeCursors::CursorDescriptor> ClusterAggregate::parseCursors(
    const std::vector<ClusterClientCursorParams::RemoteCursor>& responses) {
    std::vector<DocumentSourceMergeCursors::CursorDescriptor> cursors;
    for (const auto& response : responses) {
        invariant(0 != response.cursorResponse.getCursorId());
        invariant(response.cursorResponse.getBatch().empty());
        cursors.emplace_back(ConnectionString(response.hostAndPort),
                             response.cursorResponse.getNSS().toString(),
                             response.cursorResponse.getCursorId());
    }
    return cursors;
}

void ClusterAggregate::uassertAllShardsSupportExplain(
    const std::vector<AsyncRequestsSender::Response>& shardResults) {
    for (const auto& result : shardResults) {
        auto status = result.swResponse.getStatus();
        if (status.isOK()) {
            status = getStatusFromCommandResult(result.swResponse.getValue().data);
        }
        uassert(17403,
                str::stream() << "Shard " << result.shardId.toString() << " failed: "
                              << causedBy(status),
                status.isOK());

        uassert(17404,
                str::stream() << "Shard " << result.shardId.toString()
                              << " does not support $explain",
                result.swResponse.getValue().data.hasField("stages"));
    }
}

Status ClusterAggregate::aggPassthrough(OperationContext* opCtx,
                                        const Namespaces& namespaces,
                                        const ShardId& shardId,
                                        BSONObj cmdObj,
                                        const AggregationRequest& aggRequest,
                                        const LiteParsedPipeline& liteParsedPipeline,
                                        BSONObjBuilder* out) {
    // Temporary hack. See comment on declaration for details.
    auto swShard = Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardId);
    if (!swShard.isOK()) {
        return swShard.getStatus();
    }
    auto shard = std::move(swShard.getValue());

    // Format the command for the shard. This adds the 'fromMongos' field, wraps the command as an
    // explain if necessary, and rewrites the result into a format safe to forward to shards.
    cmdObj = Command::filterCommandRequestForPassthrough(
        createCommandForTargetedShards(aggRequest, cmdObj, nullptr));

    auto cmdResponse = uassertStatusOK(shard->runCommandWithFixedRetryAttempts(
        opCtx,
        ReadPreferenceSetting::get(opCtx),
        namespaces.executionNss.db().toString(),
        !shard->isConfig() ? appendShardVersion(std::move(cmdObj), ChunkVersion::UNSHARDED())
                           : std::move(cmdObj),
        Shard::RetryPolicy::kIdempotent));

    if (ErrorCodes::isStaleShardingError(cmdResponse.commandStatus.code())) {
        throw StaleConfigException("command failed because of stale config", cmdResponse.response);
    }

    BSONObj result;
    if (aggRequest.getExplain()) {
        // If this was an explain, then we get back an explain result object rather than a cursor.
        result = cmdResponse.response;
    } else {
        // The merging shard is remote, so if a response was received, a HostAndPort must have been
        // set.
        invariant(cmdResponse.hostAndPort);
        result = uassertStatusOK(storePossibleCursor(
            opCtx,
            shard->getId(),
            *cmdResponse.hostAndPort,
            cmdResponse.response,
            namespaces.requestedNss,
            Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
            Grid::get(opCtx)->getCursorManager(),
            liteParsedPipeline.hasChangeStream() ? TailableMode::kTailableAndAwaitData
                                                 : TailableMode::kNormal));
    }

    // First append the properly constructed writeConcernError. It will then be skipped
    // in appendElementsUnique.
    if (auto wcErrorElem = result["writeConcernError"]) {
        appendWriteConcernErrorToCmdResponse(shard->getId(), wcErrorElem, *out);
    }

    out->appendElementsUnique(Command::filterCommandReplyForPassthrough(result));

    BSONObj responseObj = out->asTempObj();
    if (ResolvedView::isResolvedViewErrorResponse(responseObj)) {
        auto resolvedView = ResolvedView::fromBSON(responseObj);

        auto resolvedAggRequest = resolvedView.asExpandedViewAggregation(aggRequest);
        auto resolvedAggCmd = resolvedAggRequest.serializeToCommandObj().toBson();
        out->resetToEmpty();

        // We pass both the underlying collection namespace and the view namespace here. The
        // underlying collection namespace is used to execute the aggregation on mongoD. Any cursor
        // returned will be registered under the view namespace so that subsequent getMore and
        // killCursors calls against the view have access.
        Namespaces nsStruct;
        nsStruct.requestedNss = namespaces.requestedNss;
        nsStruct.executionNss = resolvedView.getNamespace();

        return ClusterAggregate::runAggregate(
            opCtx, nsStruct, resolvedAggRequest, resolvedAggCmd, out);
    }

    return getStatusFromCommandResult(result);
}

}  // namespace mongo
