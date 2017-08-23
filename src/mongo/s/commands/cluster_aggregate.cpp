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

#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/operation_context.h"
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
#include "mongo/s/grid.h"
#include "mongo/s/query/cluster_client_cursor_impl.h"
#include "mongo/s/query/cluster_client_cursor_params.h"
#include "mongo/s/query/cluster_cursor_manager.h"
#include "mongo/s/query/cluster_query_knobs.h"
#include "mongo/s/query/establish_cursors.h"
#include "mongo/s/query/store_possible_cursor.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/log.h"

namespace mongo {

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
    if (pipelineForTargetedShards->isSplitForSharded()) {
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
                                    const CachedCollectionRoutingInfo& routingInfo,
                                    const BSONObj shardQuery,
                                    const BSONObj collation) {
    if (nss.isCollectionlessAggregateNS()) {
        // The pipeline begins with a stage which produces its own input and does not use an
        // underlying collection. It should be run on all shards.
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
    targetedCmd[AggregationRequest::kFromRouterName] = Value(true);

    // If 'pipelineForTargetedShards' is 'nullptr', this is an unsharded direct passthrough.
    if (pipelineForTargetedShards) {
        targetedCmd[AggregationRequest::kPipelineName] =
            Value(pipelineForTargetedShards->serialize());

        if (pipelineForTargetedShards->isSplitForSharded()) {
            targetedCmd[AggregationRequest::kNeedsMergeName] = Value(true);
            targetedCmd[AggregationRequest::kCursorName] =
                Value(DOC(AggregationRequest::kBatchSizeName << 0));
        }
    }

    // If this pipeline is not split, ensure that the write concern is propagated if present.
    if (!pipelineForTargetedShards || !pipelineForTargetedShards->isSplitForSharded()) {
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
    mergeCmd[AggregationRequest::kFromRouterName] = Value(true);
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
    CachedCollectionRoutingInfo* routingInfo,
    const BSONObj& cmdObj,
    const ReadPreferenceSetting& readPref,
    const BSONObj& shardQuery,
    const BSONObj& collation) {
    LOG(1) << "Dispatching command " << redact(cmdObj) << " to establish cursors on shards";

    std::set<ShardId> shardIds = getTargetedShards(opCtx, nss, *routingInfo, shardQuery, collation);
    std::vector<std::pair<ShardId, BSONObj>> requests;

    if (nss.isCollectionlessAggregateNS()) {
        // The pipeline begins with a stage which produces its own input and does not use an
        // underlying collection. It should be run unversioned on all shards.
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
    std::unique_ptr<Pipeline, Pipeline::Deleter> pipelineForMerging,
    std::vector<ClusterClientCursorParams::RemoteCursor> cursors) {

    ClusterClientCursorParams params(
        requestedNss,
        AuthorizationSession::get(opCtx->getClient())->getAuthenticatedUserNames(),
        ReadPreferenceSetting::get(opCtx));

    params.mergePipeline = std::move(pipelineForMerging);
    params.remotes = std::move(cursors);

    auto ccc = ClusterClientCursorImpl::make(
        opCtx, Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(), std::move(params));

    auto cursorState = ClusterCursorManager::CursorState::NotExhausted;
    BSONObjBuilder cursorResponse;

    CursorResponseBuilder responseBuilder(true, &cursorResponse);

    for (long long objCount = 0; objCount < request.getBatchSize(); ++objCount) {
        auto next = uassertStatusOK(ccc->next());

        // Check whether we have exhausted the pipeline's results.
        if (next.isEOF()) {
            cursorState = ClusterCursorManager::CursorState::Exhausted;
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

}  // namespace

Status ClusterAggregate::runAggregate(OperationContext* opCtx,
                                      const Namespaces& namespaces,
                                      const AggregationRequest& request,
                                      BSONObj cmdObj,
                                      BSONObjBuilder* result) {
    const auto catalogCache = Grid::get(opCtx)->catalogCache();

    auto executionNsRoutingInfoStatus =
        getExecutionNsRoutingInfo(opCtx, namespaces.executionNss, catalogCache);

    if (!executionNsRoutingInfoStatus.isOK()) {
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
    LiteParsedPipeline liteParsedPipeline(request);

    // TODO SERVER-29141 support forcing pipeline to run on Mongos.
    uassert(40567,
            "Unable to force mongos-only stage to run on mongos",
            liteParsedPipeline.allowedToForwardFromMongos());

    for (auto&& nss : liteParsedPipeline.getInvolvedNamespaces()) {
        const auto resolvedNsRoutingInfo =
            uassertStatusOK(catalogCache->getCollectionRoutingInfo(opCtx, nss));
        uassert(
            28769, str::stream() << nss.ns() << " cannot be sharded", !resolvedNsRoutingInfo.cm());
        resolvedNamespaces.try_emplace(nss.coll(), nss, std::vector<BSONObj>{});
    }

    // If this aggregation is on an unsharded collection, pass through to the primary shard.
    if (!executionNsRoutingInfo.cm() && !namespaces.executionNss.isCollectionlessAggregateNS() &&
        liteParsedPipeline.allowedToPassthroughFromMongos()) {
        return aggPassthrough(
            opCtx, namespaces, executionNsRoutingInfo.primary()->getId(), request, cmdObj, result);
    }

    std::unique_ptr<CollatorInterface> collation;
    if (!request.getCollation().isEmpty()) {
        collation = uassertStatusOK(CollatorFactoryInterface::get(opCtx->getServiceContext())
                                        ->makeFromBSON(request.getCollation()));
    } else if (const auto chunkMgr = executionNsRoutingInfo.cm()) {
        if (chunkMgr->getDefaultCollator()) {
            collation = chunkMgr->getDefaultCollator()->clone();
        }
    }

    boost::intrusive_ptr<ExpressionContext> mergeCtx =
        new ExpressionContext(opCtx, request, std::move(collation), std::move(resolvedNamespaces));
    mergeCtx->inRouter = true;
    // explicitly *not* setting mergeCtx->tempDir

    auto pipeline = uassertStatusOK(Pipeline::parse(request.getPipeline(), mergeCtx));
    pipeline->optimizePipeline();

    // Begin shard targeting. The process is as follows:
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

    const bool needsPrimaryShardMerge =
        (pipeline->needsPrimaryShardMerger() || internalQueryAlwaysMergeOnPrimaryShard.load());

    const auto shardQuery = pipeline->getInitialQuery();

    auto pipelineForTargetedShards = std::move(pipeline);
    std::unique_ptr<Pipeline, Pipeline::Deleter> pipelineForMerging;

    int numAttempts = 0;

    do {
        // We need to grab a new routing table at the start of each iteration, since a stale config
        // exception will invalidate the previous one.
        executionNsRoutingInfo =
            uassertStatusOK(catalogCache->getCollectionRoutingInfo(opCtx, namespaces.executionNss));

        // Determine whether we can run the entire aggregation on a single shard.
        std::set<ShardId> shardIds = getTargetedShards(opCtx,
                                                       namespaces.executionNss,
                                                       executionNsRoutingInfo,
                                                       shardQuery,
                                                       request.getCollation());

        uassert(ErrorCodes::ShardNotFound,
                "No targets were found for this aggregation. All shards were removed from the "
                "cluster mid-operation",
                shardIds.size() > 0);

        // Don't need to split pipeline if we are only targeting a single shard, unless there is a
        // stage that needs to be run on the primary shard and the single target shard is not the
        // primary.
        const bool needsSplit =
            (shardIds.size() > 1u ||
             (needsPrimaryShardMerge && *(shardIds.begin()) != executionNsRoutingInfo.primaryId()));

        const bool isSplit = pipelineForTargetedShards->isSplitForSharded();

        // If we have to run on multiple shards and the pipeline is not yet split, split it. If we
        // can run on a single shard and the pipeline is already split, reassemble it.
        if (needsSplit && !isSplit) {
            pipelineForMerging = std::move(pipelineForTargetedShards);
            pipelineForTargetedShards = pipelineForMerging->splitForSharded();
        } else if (!needsSplit && isSplit) {
            pipelineForTargetedShards->unsplitFromSharded(std::move(pipelineForMerging));
        }

        // Generate the command object for the targeted shards.
        BSONObj targetedCommand =
            createCommandForTargetedShards(request, cmdObj, pipelineForTargetedShards);

        // Explain does not produce a cursor, so instead we scatter-gather commands to the shards.
        if (mergeCtx->explain) {
            swShardResults = scatterGather(opCtx,
                                           namespaces.executionNss.db().toString(),
                                           namespaces.executionNss,
                                           targetedCommand,
                                           ReadPreferenceSetting::get(opCtx),
                                           namespaces.executionNss.isCollectionlessAggregateNS()
                                               ? ShardTargetingPolicy::BroadcastToAllShards
                                               : ShardTargetingPolicy::UseRoutingTable,
                                           shardQuery,
                                           request.getCollation(),
                                           true,
                                           false);
        } else {
            swCursors = establishShardCursors(opCtx,
                                              namespaces.executionNss,
                                              &executionNsRoutingInfo,
                                              targetedCommand,
                                              ReadPreferenceSetting::get(opCtx),
                                              shardQuery,
                                              request.getCollation());

            if (ErrorCodes::isStaleShardingError(swCursors.getStatus().code())) {
                LOG(1) << "got stale shardVersion error " << swCursors.getStatus()
                       << " while dispatching " << redact(targetedCommand) << " after "
                       << (numAttempts + 1) << " dispatch attempts";
            }
        }
    } while (++numAttempts < kMaxNumStaleVersionRetries &&
             (mergeCtx->explain ? !swShardResults.isOK() : !swCursors.isOK()));

    if (mergeCtx->explain) {
        // If we reach here, we've either succeeded in running the explain or exhausted all
        // attempts. In either case, attempt to append the explain results to the output builder.
        auto shardResults = uassertStatusOK(std::move(swShardResults));
        uassertAllShardsSupportExplain(shardResults);

        return appendExplainResults(std::move(shardResults),
                                    mergeCtx,
                                    pipelineForTargetedShards,
                                    pipelineForMerging,
                                    result);
    }

    // Retrieve the shard cursors and check whether or not we dispatched to a single shard.
    auto cursors = uassertStatusOK(std::move(swCursors));

    invariant(cursors.size() > 0);

    // If we dispatched to a single shard, store the remote cursor and return immediately.
    if (!pipelineForTargetedShards->isSplitForSharded()) {
        invariant(cursors.size() == 1);
        auto executorPool = Grid::get(opCtx)->getExecutorPool();
        const BSONObj reply = uassertStatusOK(storePossibleCursor(
            opCtx,
            cursors[0].shardId,
            cursors[0].hostAndPort,
            cursors[0].cursorResponse.toBSON(CursorResponse::ResponseType::InitialResponse),
            namespaces.requestedNss,
            executorPool->getArbitraryExecutor(),
            Grid::get(opCtx)->getCursorManager()));

        return appendCursorResponseToCommandResult(cursors[0].shardId, reply, result);
    }

    // If we reach here, we have a merge pipeline to dispatch.
    invariant(pipelineForMerging);

    // We need a DocumentSourceMergeCursors regardless of whether we merge on mongoS or on a shard.
    pipelineForMerging->addInitialSource(
        DocumentSourceMergeCursors::create(parseCursors(cursors), mergeCtx));

    // First, check whether we can merge on the mongoS.
    if (pipelineForMerging->canRunOnMongos() && !internalQueryProhibitMergingOnMongoS.load()) {
        // Register the new mongoS cursor, and retrieve the initial batch of results.
        auto cursorResponse = establishMergingMongosCursor(opCtx,
                                                           request,
                                                           namespaces.requestedNss,
                                                           std::move(pipelineForMerging),
                                                           std::move(cursors));

        // We don't need to storePossibleCursor or propagate writeConcern errors; an $out pipeline
        // can never run on mongoS. Filter the command response and return immediately.
        Command::filterCommandReplyForPassthrough(cursorResponse, result);
        return getStatusFromCommandResult(result->asTempObj());
    }

    // If we cannot merge on mongoS, establish the merge cursor on a shard.
    auto mergeCmdObj = createCommandForMergingShard(request, mergeCtx, cmdObj, pipelineForMerging);

    auto mergeResponse = uassertStatusOK(establishMergingShardCursor(
        opCtx,
        namespaces.executionNss,
        cursors,
        mergeCmdObj,
        boost::optional<ShardId>{needsPrimaryShardMerge, executionNsRoutingInfo.primaryId()}));

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
                                        const AggregationRequest& aggRequest,
                                        BSONObj cmdObj,
                                        BSONObjBuilder* out) {
    // Temporary hack. See comment on declaration for details.
    auto swShard = Grid::get(opCtx)->shardRegistry()->getShard(opCtx, shardId);
    if (!swShard.isOK()) {
        return swShard.getStatus();
    }
    auto shard = std::move(swShard.getValue());

    // Format the command for the shard. This adds the 'fromRouter' field, wraps the command as an
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
        throw RecvStaleConfigException("command failed because of stale config",
                                       cmdResponse.response);
    }

    BSONObj result;
    if (aggRequest.getExplain()) {
        // If this was an explain, then we get back an explain result object rather than a cursor.
        result = cmdResponse.response;
    } else {
        // The merging shard is remote, so if a response was received, a HostAndPort must have been
        // set.
        invariant(cmdResponse.hostAndPort);
        result = uassertStatusOK(
            storePossibleCursor(opCtx,
                                shard->getId(),
                                *cmdResponse.hostAndPort,
                                cmdResponse.response,
                                namespaces.requestedNss,
                                Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
                                Grid::get(opCtx)->getCursorManager()));
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
