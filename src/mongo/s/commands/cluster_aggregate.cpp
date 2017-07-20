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

#include "mongo/db/client.h"
#include "mongo/db/commands.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/pipeline/document_source_out.h"
#include "mongo/db/pipeline/expression_context.h"
#include "mongo/db/pipeline/lite_parsed_pipeline.h"
#include "mongo/db/pipeline/pipeline.h"
#include "mongo/db/query/collation/collator_factory_interface.h"
#include "mongo/db/views/resolved_view.h"
#include "mongo/db/views/view.h"
#include "mongo/executor/task_executor_pool.h"
#include "mongo/rpc/get_status_from_command_result.h"
#include "mongo/s/catalog_cache.h"
#include "mongo/s/client/shard_connection.h"
#include "mongo/s/client/shard_registry.h"
#include "mongo/s/commands/cluster_commands_helpers.h"
#include "mongo/s/grid.h"
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

StatusWith<std::vector<ClusterClientCursorParams::RemoteCursor>>
establishCursorsRetryOnStaleVersion(OperationContext* opCtx,
                                    const NamespaceString& nss,
                                    const BSONObj& cmdObj,
                                    const ReadPreferenceSetting& readPref,
                                    const BSONObj& query,
                                    const BSONObj& collation) {
    StatusWith<std::vector<ClusterClientCursorParams::RemoteCursor>> swCursors(
        (std::vector<ClusterClientCursorParams::RemoteCursor>()));

    LOG(1) << "Dispatching command " << redact(cmdObj) << " to establish cursors on shards";

    int numAttempts = 0;
    do {
        auto routingInfo =
            uassertStatusOK(Grid::get(opCtx)->catalogCache()->getCollectionRoutingInfo(opCtx, nss));

        std::vector<std::pair<ShardId, BSONObj>> requests;

        if (nss.isCollectionlessAggregateNS()) {
            // The pipeline begins with a stage which produces its own input and does not use an
            // underlying collection. It should be run unversioned on all shards.
            std::vector<ShardId> shardIds;
            Grid::get(opCtx)->shardRegistry()->getAllShardIds(&shardIds);
            for (auto& shardId : shardIds) {
                requests.emplace_back(std::move(shardId), cmdObj);
            }
        } else if (routingInfo.cm()) {
            // The collection is sharded. Use the routing table to decide which shards to target
            // based on the query and collation, and build versioned requests for them.
            std::set<ShardId> shardIds;
            routingInfo.cm()->getShardIdsForQuery(opCtx, query, collation, &shardIds);
            for (auto& shardId : shardIds) {
                auto versionedCmdObj =
                    appendShardVersion(cmdObj, routingInfo.cm()->getVersion(shardId));
                requests.emplace_back(std::move(shardId), std::move(versionedCmdObj));
            }
        } else {
            // The collection is unsharded. Target only the primary shard for the database.
            // Don't append shard version info when contacting the config servers.
            requests.emplace_back(routingInfo.primaryId(),
                                  !routingInfo.primary()->isConfig()
                                      ? appendShardVersion(cmdObj, ChunkVersion::UNSHARDED())
                                      : cmdObj);
        }

        // If we reach this point, we're trying to establish cursors on multiple shards, meaning the
        // execution namespace is sharded. Since views cannot be sharded, there's no way we can
        // receive a viewDefinition in a response from a shard.
        BSONObj* viewDefinitionOut = nullptr;
        swCursors = establishCursors(opCtx,
                                     Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
                                     nss,
                                     readPref,
                                     requests,
                                     false /* do not allow partial results */,
                                     viewDefinitionOut /* can't receive view definition */);
        ++numAttempts;

        // If any shard returned a stale shardVersion error, invalidate the routing table cache.
        // This will cause the cache to be refreshed the next time it is accessed.
        if (ErrorCodes::isStaleShardingError(swCursors.getStatus().code())) {
            Grid::get(opCtx)->catalogCache()->onStaleConfigError(std::move(routingInfo));
            LOG(1) << "got stale shardVersion error " << swCursors.getStatus()
                   << " while dispatching " << redact(cmdObj) << " after " << numAttempts
                   << " dispatch attempts";
        }
    } while (numAttempts < kMaxNumStaleVersionRetries && !swCursors.isOK());

    if (!swCursors.isOK()) {
        return swCursors.getStatus();
    }
    return std::move(swCursors.getValue());
}

}  // namespace

Status ClusterAggregate::runAggregate(OperationContext* opCtx,
                                      const Namespaces& namespaces,
                                      const AggregationRequest& request,
                                      BSONObj cmdObj,
                                      BSONObjBuilder* result) {
    auto const catalogCache = Grid::get(opCtx)->catalogCache();

    auto executionNsRoutingInfoStatus =
        catalogCache->getCollectionRoutingInfo(opCtx, namespaces.executionNss);
    if (!executionNsRoutingInfoStatus.isOK()) {
        appendEmptyResultSet(
            *result, executionNsRoutingInfoStatus.getStatus(), namespaces.requestedNss.ns());
        return Status::OK();
    }

    const auto& executionNsRoutingInfo = executionNsRoutingInfoStatus.getValue();

    // Determine the appropriate collation and 'resolve' involved namespaces to make the
    // ExpressionContext.

    // We won't try to execute anything on a mongos, but we still have to populate this map so that
    // any $lookups, etc. will be able to have a resolved view definition. It's okay that this is
    // incorrect, we will repopulate the real resolved namespace map on the mongod. Note that we
    // need to check if any involved collections are sharded before forwarding an aggregation
    // command on an unsharded collection.
    StringMap<ExpressionContext::ResolvedNamespace> resolvedNamespaces;
    LiteParsedPipeline liteParsedPipeline(request);

    // TODO SERVER-29141 support $changeNotification on mongos.
    uassert(40567,
            "$changeNotification is not yet supported on mongos",
            !liteParsedPipeline.hasChangeNotification());

    for (auto&& nss : liteParsedPipeline.getInvolvedNamespaces()) {
        const auto resolvedNsRoutingInfo =
            uassertStatusOK(catalogCache->getCollectionRoutingInfo(opCtx, nss));
        uassert(
            28769, str::stream() << nss.ns() << " cannot be sharded", !resolvedNsRoutingInfo.cm());
        resolvedNamespaces.try_emplace(nss.coll(), nss, std::vector<BSONObj>{});
    }

    if (!executionNsRoutingInfo.cm() && !namespaces.executionNss.isCollectionlessAggregateNS()) {
        return aggPassthrough(
            opCtx, namespaces, executionNsRoutingInfo.primary()->getId(), request, cmdObj, result);
    }
    const auto chunkMgr = executionNsRoutingInfo.cm();

    std::unique_ptr<CollatorInterface> collation;
    if (!request.getCollation().isEmpty()) {
        collation = uassertStatusOK(CollatorFactoryInterface::get(opCtx->getServiceContext())
                                        ->makeFromBSON(request.getCollation()));
    } else if (chunkMgr && chunkMgr->getDefaultCollator()) {
        collation = chunkMgr->getDefaultCollator()->clone();
    }

    boost::intrusive_ptr<ExpressionContext> mergeCtx =
        new ExpressionContext(opCtx, request, std::move(collation), std::move(resolvedNamespaces));
    mergeCtx->inRouter = true;
    // explicitly *not* setting mergeCtx->tempDir

    // Parse and optimize the pipeline specification.
    auto pipeline = Pipeline::parse(request.getPipeline(), mergeCtx);
    if (!pipeline.isOK()) {
        return pipeline.getStatus();
    }

    pipeline.getValue()->optimizePipeline();

    // If the first $match stage is an exact match on the shard key (with a simple collation or no
    // string matching), we only have to send it to one shard, so send the command to that shard.
    const bool singleShard = !namespaces.executionNss.isCollectionlessAggregateNS() && [&]() {
        invariant(chunkMgr);

        BSONObj firstMatchQuery = pipeline.getValue()->getInitialQuery();
        BSONObj shardKeyMatches = uassertStatusOK(
            chunkMgr->getShardKeyPattern().extractShardKeyFromQuery(opCtx, firstMatchQuery));

        if (shardKeyMatches.isEmpty()) {
            return false;
        }

        try {
            chunkMgr->findIntersectingChunk(shardKeyMatches, request.getCollation());
            return true;
        } catch (const DBException&) {
            return false;
        }
    }();

    // Don't need to split pipeline if the first $match is an exact match on shard key, unless
    // there is a stage that needs to be run on the primary shard.
    const bool needPrimaryShardMerger = pipeline.getValue()->needsPrimaryShardMerger();
    const bool needSplit = !singleShard || needPrimaryShardMerger;

    // Split the pipeline into pieces for mongod(s) and this mongos. It is illegal to use 'pipeline'
    // after this point.
    std::unique_ptr<Pipeline, Pipeline::Deleter> pipelineForTargetedShards;
    std::unique_ptr<Pipeline, Pipeline::Deleter> pipelineForMergingShard;
    if (needSplit) {
        pipelineForTargetedShards = pipeline.getValue()->splitForSharded();
        pipelineForMergingShard = std::move(pipeline.getValue());
    } else {
        pipelineForTargetedShards = std::move(pipeline.getValue());
    }

    // Create the command for the shards. The 'fromRouter' field means produce output to be
    // merged.
    MutableDocument targetedCommandBuilder(request.serializeToCommandObj());
    targetedCommandBuilder[AggregationRequest::kPipelineName] =
        Value(pipelineForTargetedShards->serialize());
    if (needSplit) {
        targetedCommandBuilder[AggregationRequest::kFromRouterName] = Value(true);
        targetedCommandBuilder[AggregationRequest::kCursorName] =
            Value(DOC(AggregationRequest::kBatchSizeName << 0));
    }

    // If this is a request for an aggregation explain, then we must wrap the aggregate inside an
    // explain command.
    if (mergeCtx->explain) {
        targetedCommandBuilder.reset(
            wrapAggAsExplain(targetedCommandBuilder.freeze(), *mergeCtx->explain));
    }

    BSONObj targetedCommand = targetedCommandBuilder.freeze().toBson();
    BSONObj shardQuery = pipelineForTargetedShards->getInitialQuery();

    if (mergeCtx->explain) {
        auto shardResults =
            uassertStatusOK(scatterGather(opCtx,
                                          namespaces.executionNss.db().toString(),
                                          namespaces.executionNss,
                                          targetedCommand,
                                          ReadPreferenceSetting::get(opCtx),
                                          namespaces.executionNss.isCollectionlessAggregateNS()
                                              ? ShardTargetingPolicy::BroadcastToAllShards
                                              : ShardTargetingPolicy::UseRoutingTable,
                                          shardQuery,
                                          request.getCollation()));

        // This must be checked before we start modifying result.
        uassertAllShardsSupportExplain(shardResults);

        if (needSplit) {
            *result << "needsPrimaryShardMerger" << needPrimaryShardMerger << "splitPipeline"
                    << Document{{"shardsPart",
                                 pipelineForTargetedShards->writeExplainOps(*mergeCtx->explain)},
                                {"mergerPart",
                                 pipelineForMergingShard->writeExplainOps(*mergeCtx->explain)}};
        } else {
            *result << "splitPipeline" << BSONNULL;
        }

        BSONObjBuilder shardExplains(result->subobjStart("shards"));
        for (const auto& result : shardResults) {
            invariant(result.shardHostAndPort);
            shardExplains.append(result.shardId.toString(),
                                 BSON("host" << result.shardHostAndPort->toString() << "stages"
                                             << result.swResponse.getValue().data["stages"]));
        }

        return Status::OK();
    }

    auto cursors =
        uassertStatusOK(establishCursorsRetryOnStaleVersion(opCtx,
                                                            namespaces.executionNss,
                                                            targetedCommand,
                                                            ReadPreferenceSetting::get(opCtx),
                                                            shardQuery,
                                                            request.getCollation()));

    if (!needSplit) {
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
        Command::filterCommandReplyForPassthrough(reply, result);
        return getStatusFromCommandResult(reply);
    }

    pipelineForMergingShard->addInitialSource(
        DocumentSourceMergeCursors::create(parseCursors(cursors), mergeCtx));

    MutableDocument mergeCmd(request.serializeToCommandObj());
    mergeCmd["pipeline"] = Value(pipelineForMergingShard->serialize());
    mergeCmd["cursor"] = Value(cmdObj["cursor"]);

    if (cmdObj.hasField(QueryRequest::cmdOptionMaxTimeMS)) {
        mergeCmd[QueryRequest::cmdOptionMaxTimeMS] =
            Value(cmdObj[QueryRequest::cmdOptionMaxTimeMS]);
    }

    mergeCmd.setField("writeConcern", Value(cmdObj["writeConcern"]));
    mergeCmd.setField("readConcern", Value(cmdObj["readConcern"]));

    // If the user didn't specify a collation already, make sure there's a collation attached to
    // the merge command, since the merging shard may not have the collection metadata.
    if (mergeCmd.peek()["collation"].missing()) {
        mergeCmd.setField("collation",
                          mergeCtx->getCollator()
                              ? Value(mergeCtx->getCollator()->getSpec().toBSON())
                              : Value(Document{CollationSpec::kSimpleSpec}));
    }

    auto mergeCmdObj = mergeCmd.freeze().toBson();

    std::string outputNsOrEmpty;
    if (DocumentSourceOut* out =
            dynamic_cast<DocumentSourceOut*>(pipelineForMergingShard->getSources().back().get())) {
        outputNsOrEmpty = out->getOutputNs().ns();
    }

    // Run merging command on random shard, unless a stage needs the primary shard.
    auto& prng = opCtx->getClient()->getPrng();
    const auto mergingShardId =
        (needPrimaryShardMerger || internalQueryAlwaysMergeOnPrimaryShard.load())
        ? uassertStatusOK(catalogCache->getDatabase(opCtx, namespaces.executionNss.db()))
              .primaryId()
        : cursors[prng.nextInt32(cursors.size())].shardId;
    const auto mergingShard =
        uassertStatusOK(Grid::get(opCtx)->shardRegistry()->getShard(opCtx, mergingShardId));

    auto response = uassertStatusOK(
        mergingShard->runCommandWithFixedRetryAttempts(opCtx,
                                                       ReadPreferenceSetting::get(opCtx),
                                                       namespaces.executionNss.db().toString(),
                                                       mergeCmdObj,
                                                       Shard::RetryPolicy::kNoRetry));

    // The merging shard is remote, so if a response was received, a HostAndPort must have been set.
    invariant(response.hostAndPort);
    auto mergedResults = uassertStatusOK(
        storePossibleCursor(opCtx,
                            mergingShardId,
                            *response.hostAndPort,
                            response.response,
                            namespaces.requestedNss,
                            Grid::get(opCtx)->getExecutorPool()->getArbitraryExecutor(),
                            Grid::get(opCtx)->getCursorManager()));

    if (auto wcErrorElem = mergedResults["writeConcernError"]) {
        appendWriteConcernErrorToCmdResponse(mergingShardId, wcErrorElem, *result);
    }

    // Copy output from merging (primary) shard to the output object from our command.
    // Also, propagates errmsg and code if ok == false.
    result->appendElementsUnique(Command::filterCommandReplyForPassthrough(mergedResults));

    return getStatusFromCommandResult(result->asTempObj());
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

    // If this is an explain, we need to re-create the explain command which will be forwarded to
    // the shards.
    if (aggRequest.getExplain()) {
        auto explainCmdObj =
            wrapAggAsExplain(aggRequest.serializeToCommandObj(), *aggRequest.getExplain());
        cmdObj = explainCmdObj.toBson();
    }

    cmdObj = Command::filterCommandRequestForPassthrough(cmdObj);
    auto cmdResponse = uassertStatusOK(shard->runCommandWithFixedRetryAttempts(
        opCtx,
        ReadPreferenceSetting::get(opCtx),
        namespaces.executionNss.db().toString(),
        !shard->isConfig() ? appendShardVersion(std::move(cmdObj), ChunkVersion::UNSHARDED())
                           : std::move(cmdObj),
        Shard::RetryPolicy::kNoRetry));

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
