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
#include "mongo/s/query/store_possible_cursor.h"
#include "mongo/s/stale_exception.h"
#include "mongo/util/log.h"

namespace mongo {

namespace {
Status appendExplainResults(
    const std::vector<AsyncRequestsSender::Response>& shardResults,
    const boost::intrusive_ptr<ExpressionContext>& mergeCtx,
    const std::unique_ptr<Pipeline, PipelineDeleter>& pipelineForTargetedShards,
    const std::unique_ptr<Pipeline, PipelineDeleter>& pipelineForMerging,
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
    result->appendElementsUnique(CommandHelpers::filterCommandReplyForPassthrough(cursorResponse));
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

BSONObj createCommandForMergingShard(
    const AggregationRequest& request,
    const boost::intrusive_ptr<ExpressionContext>& mergeCtx,
    const BSONObj originalCmdObj,
    const std::unique_ptr<Pipeline, PipelineDeleter>& pipelineForMerging) {
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
    auto executionNss = namespaces.executionNss;

    auto executionNsRoutingInfoStatus =
        getExecutionNsRoutingInfo(opCtx, executionNss, catalogCache);

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

    // We may not try to execute anything on mongos, but we still have to populate this map so that
    // any $lookups, etc. will be able to have a resolved view definition when they are parsed.
    // TODO: SERVER-32548 will add support for lookup against a sharded view, so this map needs to
    // be correct to determine whether the aggregate should be passthrough or sent to all shards.
    StringMap<ExpressionContext::ResolvedNamespace> resolvedNamespaces;
    bool involvesShardedCollections = false;

    for (auto&& nss : liteParsedPipeline.getInvolvedNamespaces()) {
        const auto resolvedNsRoutingInfo =
            uassertStatusOK(catalogCache->getCollectionRoutingInfo(opCtx, nss));
        resolvedNamespaces.try_emplace(nss.coll(), nss, std::vector<BSONObj>{});
        if (resolvedNsRoutingInfo.cm()) {
            involvesShardedCollections = true;
        }
    }

    // A pipeline is allowed to passthrough to the primary shard iff the following conditions are
    // met:
    //
    // 1. The namespace of the aggregate and any other involved namespaces are unsharded.
    // 2. Is allowed to be forwarded to shards.
    // 3. Does not need to run on all shards.
    // 4. Doesn't need transformation via DocumentSource::serialize().
    if (!executionNsRoutingInfo.cm() &&
        !PipelineS::mustRunOnAllShards(executionNss, executionNsRoutingInfo, liteParsedPipeline) &&
        liteParsedPipeline.allowedToForwardFromMongos() &&
        liteParsedPipeline.allowedToPassthroughFromMongos() && !involvesShardedCollections) {
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
            executionNsRoutingInfo.primary().get(), executionNss);
        if (!collationObj.isEmpty()) {
            collation = uassertStatusOK(CollatorFactoryInterface::get(opCtx->getServiceContext())
                                            ->makeFromBSON(collationObj));
        }
    }

    boost::intrusive_ptr<ExpressionContext> mergeCtx =
        new ExpressionContext(opCtx,
                              request,
                              std::move(collation),
                              std::make_shared<PipelineS::MongoSInterface>(),
                              std::move(resolvedNamespaces));
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

        auto cursorResponse = PipelineS::establishMergingMongosCursor(opCtx,
                                                                      request,
                                                                      namespaces.requestedNss,
                                                                      cmdObj,
                                                                      liteParsedPipeline,
                                                                      std::move(pipeline),
                                                                      {});
        CommandHelpers::filterCommandReplyForPassthrough(cursorResponse, result);
        return getStatusFromCommandResult(result->asTempObj());
    }

    auto dispatchResults = uassertStatusOK(PipelineS::dispatchShardPipeline(
        mergeCtx, executionNss, cmdObj, request, liteParsedPipeline, std::move(pipeline)));

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
            PipelineS::establishMergingMongosCursor(opCtx,
                                                    request,
                                                    namespaces.requestedNss,
                                                    dispatchResults.commandForTargetedShards,
                                                    liteParsedPipeline,
                                                    std::move(mergingPipeline),
                                                    std::move(dispatchResults.remoteCursors));

        // We don't need to storePossibleCursor or propagate writeConcern errors; an $out pipeline
        // can never run on mongoS. Filter the command response and return immediately.
        CommandHelpers::filterCommandReplyForPassthrough(cursorResponse, result);
        return getStatusFromCommandResult(result->asTempObj());
    }

    // If we cannot merge on mongoS, establish the merge cursor on a shard.
    mergingPipeline->addInitialSource(
        DocumentSourceMergeCursors::create(parseCursors(dispatchResults.remoteCursors), mergeCtx));
    auto mergeCmdObj = createCommandForMergingShard(request, mergeCtx, cmdObj, mergingPipeline);

    auto mergeResponse = uassertStatusOK(
        establishMergingShardCursor(opCtx,
                                    executionNss,
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
    cmdObj = CommandHelpers::filterCommandRequestForPassthrough(
        PipelineS::createCommandForTargetedShards(aggRequest, cmdObj, nullptr));

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

    out->appendElementsUnique(CommandHelpers::filterCommandReplyForPassthrough(result));

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
